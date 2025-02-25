//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2004-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/managed_heap_memory.hpp>
#include <boost/interprocess/containers/map.hpp>
#include <boost/interprocess/containers/set.hpp>
#include <boost/interprocess/allocators/node_allocator.hpp>
#include <boost/interprocess/detail/os_thread_functions.hpp>
// intrusive/detail
#include <boost/intrusive/detail/minimal_pair_header.hpp>
#include <boost/intrusive/detail/minimal_less_equal_header.hpp>

#include <boost/move/unique_ptr.hpp>

#include <cstddef>
#include <memory>
#include <iostream>
#include <vector>
#include <exception>
#include <limits>

#include "get_process_id_name.hpp"
#include "named_creation_template.hpp"

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  This example tests the process shared message queue.                      //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

using namespace boost::interprocess;

//This test inserts messages with different priority and marks them with a
//time-stamp to check if receiver obtains highest priority messages first and
//messages with same priority are received in fifo order
bool test_priority_order()
{
   message_queue::remove(test::get_process_id_name());
   {
      message_queue mq1
         (open_or_create, test::get_process_id_name(), 100, sizeof(std::size_t)),
         mq2
         (open_or_create, test::get_process_id_name(), 100, sizeof(std::size_t));

      //We test that the queue is ordered by priority and in the
      //same priority, is a FIFO
      message_queue::size_type recvd = 0;
      unsigned int priority = 0;
      std::size_t tstamp;
      unsigned int priority_prev;
      std::size_t  tstamp_prev;

      //We will send 100 message with priority 0-9
      //The message will contain the timestamp of the message
      for(std::size_t i = 0; i < 100; ++i){
         tstamp = i;
         mq1.send(&tstamp, sizeof(tstamp), (unsigned int)(i%10));
      }

      priority_prev = (std::numeric_limits<unsigned int>::max)();
      tstamp_prev = 0;

      //Receive all messages and test those are ordered
      //by priority and by FIFO in the same priority
      for(std::size_t i = 0; i < 100; ++i){
         mq1.receive(&tstamp, sizeof(tstamp), recvd, priority);
         if(priority > priority_prev)
            return false;
         if(priority == priority_prev &&
            tstamp   <= tstamp_prev){
            return false;
         }
         priority_prev  = priority;
         tstamp_prev    = tstamp;
      }

      //Now retry it with different priority order
      for(std::size_t i = 0; i < 100; ++i){
         tstamp = i;
         mq1.send(&tstamp, sizeof(tstamp), (unsigned int)(9 - i%10));
      }

      priority_prev = (std::numeric_limits<unsigned int>::max)();
      tstamp_prev = 0;

      //Receive all messages and test those are ordered
      //by priority and by FIFO in the same priority
      for(std::size_t i = 0; i < 100; ++i){
         mq1.receive(&tstamp, sizeof(tstamp), recvd, priority);
         if(priority > priority_prev)
            return false;
         if(priority == priority_prev &&
            tstamp   <= tstamp_prev){
            return false;
         }
         priority_prev  = priority;
         tstamp_prev    = tstamp;
      }
   }
   message_queue::remove(test::get_process_id_name());
   return true;
}

//[message_queue_test_test_serialize_db
//This test creates a in memory data-base using Interprocess machinery and
//serializes it through a message queue. Then rebuilds the data-base in
//another buffer and checks it against the original data-base
bool test_serialize_db()
{
   //Typedef data to create a Interprocess map
   typedef std::pair<const std::size_t, std::size_t> MyPair;
   typedef std::less<std::size_t>   MyLess;
   typedef node_allocator<MyPair, managed_external_buffer::segment_manager>
      node_allocator_t;
   typedef map<std::size_t,
               std::size_t,
               std::less<std::size_t>,
               node_allocator_t>
               MyMap;

   //Some constants
   const std::size_t BufferSize  = 65536;
   const std::size_t MaxMsgSize  = 100;

   //Allocate a memory buffer to hold the destiny database using vector<char>
   std::vector<char> buffer_destiny(BufferSize, 0);

   message_queue::remove(test::get_process_id_name());
   {
      //Create the message-queues
      message_queue mq1(create_only, test::get_process_id_name(), 1, MaxMsgSize);

      //Open previously created message-queue simulating other process
      message_queue mq2(open_only, test::get_process_id_name());

      //A managed heap memory to create the origin database
      managed_heap_memory db_origin(buffer_destiny.size());

      //Construct the map in the first buffer
      MyMap *map1 = db_origin.construct<MyMap>("MyMap")
                                       (MyLess(),
                                       db_origin.get_segment_manager());
      if(!map1)
         return false;

      //Fill map1 until is full
      BOOST_TRY{
         std::size_t i = 0;
         while(1){
            (*map1)[i] = i;
            ++i;
         }
      }
      BOOST_CATCH(boost::interprocess::bad_alloc &){} BOOST_CATCH_END

      //Data control data sending through the message queue
      std::size_t sent = 0;
      message_queue::size_type recvd = 0;
      message_queue::size_type total_recvd = 0;
      unsigned int priority;

      //Send whole first buffer through the mq1, read it
      //through mq2 to the second buffer
      while(1){
         //Send a fragment of buffer1 through mq1
       std::size_t bytes_to_send = MaxMsgSize < (db_origin.get_size() - sent) ?
                                       MaxMsgSize : (db_origin.get_size() - sent);
         mq1.send( &static_cast<char*>(db_origin.get_address())[sent]
               , bytes_to_send
               , 0);
         sent += bytes_to_send;
         //Receive the fragment through mq2 to buffer_destiny
       mq2.receive( &buffer_destiny[total_recvd]
                , BufferSize - recvd
                  , recvd
                  , priority);
         total_recvd += recvd;

         //Check if we have received all the buffer
         if(total_recvd == BufferSize){
            break;
         }
      }

      //The buffer will contain a copy of the original database
      //so let's interpret the buffer with managed_external_buffer
      managed_external_buffer db_destiny(open_only, &buffer_destiny[0], BufferSize);

      //Let's find the map
      std::pair<MyMap *, managed_external_buffer::size_type> ret = db_destiny.find<MyMap>("MyMap");
      MyMap *map2 = ret.first;

      //Check if we have found it
      if(!map2){
         return false;
      }

      //Check if it is a single variable (not an array)
      if(ret.second != 1){
         return false;
      }

      //Now let's compare size
      if(map1->size() != map2->size()){
         return false;
      }

      //Now let's compare all db values
     MyMap::size_type num_elements = map1->size();
     for(std::size_t i = 0; i < num_elements; ++i){
         if((*map1)[i] != (*map2)[i]){
            return false;
         }
      }

      //Destroy maps from db-s
      db_origin.destroy_ptr(map1);
      db_destiny.destroy_ptr(map2);
   }
   message_queue::remove(test::get_process_id_name());
   return true;
}
//]

static const int MsgSize = 10;
static const int NumMsg  = 1000;
static char msgsend [10];
static char msgrecv [10];

static boost::interprocess::message_queue *pmessage_queue;

void receiver()
{
   boost::interprocess::message_queue::size_type recvd_size;
   unsigned int priority;
   int nummsg = NumMsg;

   while(nummsg--){
      pmessage_queue->receive(msgrecv, MsgSize, recvd_size, priority);
   }
}

bool test_buffer_overflow()
{
   boost::interprocess::message_queue::remove(test::get_process_id_name());
   {
      boost::movelib::unique_ptr<boost::interprocess::message_queue>
         ptr(new boost::interprocess::message_queue
               (create_only, test::get_process_id_name(), 10, 10));
      pmessage_queue = ptr.get();

      //Launch the receiver thread
      boost::interprocess::ipcdetail::OS_thread_t thread;
      boost::interprocess::ipcdetail::thread_launch(thread, &receiver);
      boost::interprocess::ipcdetail::thread_yield();

      int nummsg = NumMsg;

      while(nummsg--){
         pmessage_queue->send(msgsend, MsgSize, 0);
      }

      boost::interprocess::ipcdetail::thread_join(thread);
   }
   boost::interprocess::message_queue::remove(test::get_process_id_name());
   return true;
}


//////////////////////////////////////////////////////////////////////////////
//
// test_multi_sender_receiver is based on Alexander (aalutov's)
// testcase for ticket #9221. Many thanks.
//
//////////////////////////////////////////////////////////////////////////////

static boost::interprocess::message_queue *global_queue = 0;
//We'll send MULTI_NUM_MSG_PER_SENDER messages per sender
static const int MULTI_NUM_MSG_PER_SENDER = 10000;
//Message queue message capacity
static const int MULTI_QUEUE_SIZE = (MULTI_NUM_MSG_PER_SENDER - 1)/MULTI_NUM_MSG_PER_SENDER + 1;
//We'll launch MULTI_THREAD_COUNT senders and MULTI_THREAD_COUNT receivers
static const int MULTI_THREAD_COUNT = 10;

static void multisend()
{
   char buff;
   for (int i = 0; i < MULTI_NUM_MSG_PER_SENDER; i++) {
      global_queue->send(&buff, 1, 0);
   }
   global_queue->send(&buff, 0, 0);
   //std::cout<<"writer thread complete"<<std::endl;
}

static void multireceive()
{
   char buff;
   size_t size;
   int received_msgs = 0;
   unsigned int priority;
   do {
      global_queue->receive(&buff, 1, size, priority);
      ++received_msgs;
   } while (size > 0);
   --received_msgs;
   //std::cout << "reader thread complete, read msgs: " << received_msgs << std::endl;
}


bool test_multi_sender_receiver()
{
   bool ret = true;
   //std::cout << "Testing multi-sender / multi-receiver " << std::endl;
   BOOST_TRY {
      boost::interprocess::message_queue::remove(test::get_process_id_name());
      boost::interprocess::message_queue mq
         (boost::interprocess::open_or_create, test::get_process_id_name(), MULTI_QUEUE_SIZE, 1);
      global_queue = &mq;
      std::vector<boost::interprocess::ipcdetail::OS_thread_t> threads(MULTI_THREAD_COUNT*2);

      //Launch senders receiver thread
      for (int i = 0; i < MULTI_THREAD_COUNT; i++) {
         boost::interprocess::ipcdetail::thread_launch
            (threads[i], &multisend);
      }

      for (int i = 0; i < MULTI_THREAD_COUNT; i++) {
         boost::interprocess::ipcdetail::thread_launch
            (threads[MULTI_THREAD_COUNT+i], &multireceive);
      }

      for (int i = 0; i < MULTI_THREAD_COUNT*2; i++) {
         boost::interprocess::ipcdetail::thread_join(threads[i]);
         //std::cout << "Joined thread " << i << std::endl;
      }
   }
   BOOST_CATCH(std::exception &e) {
      std::cout << "error " << e.what() << std::endl;
      ret = false;
   } BOOST_CATCH_END
   boost::interprocess::message_queue::remove(test::get_process_id_name());
   return ret;
}

class msg_queue_named_test_wrapper
   : public test::named_sync_deleter<message_queue>, public message_queue
{
   public:

   msg_queue_named_test_wrapper(create_only_t)
      :  message_queue(create_only, test::get_process_id_name(), 10, 10)
   {}

   msg_queue_named_test_wrapper(open_only_t)
      :  message_queue(open_only, test::get_process_id_name())
   {}

   msg_queue_named_test_wrapper(open_or_create_t)
      :  message_queue(open_or_create, test::get_process_id_name(), 10, 10)
   {}

   ~msg_queue_named_test_wrapper()
   {}
};

#if defined(BOOST_INTERPROCESS_WCHAR_NAMED_RESOURCES)

class msg_queue_named_test_wrapper_w
   : public test::named_sync_deleter_w<message_queue>, public message_queue
{
   public:

   template <class CharT>
   msg_queue_named_test_wrapper_w(create_only_t)
      :  message_queue(create_only, test::get_process_id_wname(), 10, 10)
   {}

   msg_queue_named_test_wrapper_w(open_only_t)
      :  message_queue(open_only, test::get_process_id_wname())
   {}

   msg_queue_named_test_wrapper_w(open_or_create_t)
      :  message_queue(open_or_create, test::get_process_id_wname(), 10, 10)
   {}

   ~msg_queue_named_test_wrapper_w()
   {}
};

#endif   //defined(BOOST_INTERPROCESS_WCHAR_NAMED_RESOURCES)


int main ()
{
   int ret = 0;
   BOOST_TRY{
      message_queue::remove(test::get_process_id_name());
      test::test_named_creation<msg_queue_named_test_wrapper>();
      #if defined(BOOST_INTERPROCESS_WCHAR_NAMED_RESOURCES)
      test::test_named_creation<msg_queue_named_test_wrapper>();
      #endif

      if(!test_priority_order()){
         return 1;
      }

      if(!test_serialize_db()){
         return 1;
      }

      if(!test_buffer_overflow()){
         return 1;
      }

      if(!test_multi_sender_receiver()){
         return 1;
      }
   }
   BOOST_CATCH(std::exception &ex) {
      std::cout << ex.what() << std::endl;
      ret = 1;
   } BOOST_CATCH_END
   
   message_queue::remove(test::get_process_id_name());
   return ret;
}

