#include "l1_cache_cntlr.h"
#include "l2_cache_cntlr.h"
#include "memory_manager.h"
#include "log.h"

namespace PrL1PrL2DramDirectoryMSI
{

L2CacheCntlr::L2CacheCntlr(MemoryManager* memory_manager,
                           L1CacheCntlr* l1_cache_cntlr,
                           AddressHomeLookup* dram_directory_home_lookup,
                           Semaphore* user_thread_sem,
                           Semaphore* network_thread_sem,
                           UInt32 cache_line_size,
                           UInt32 l2_cache_size,
                           UInt32 l2_cache_associativity,
                           string l2_cache_replacement_policy,
                           UInt32 l2_cache_access_delay,
                           bool l2_cache_track_miss_types,
                           volatile float frequency)
   : _memory_manager(memory_manager)
   , _l1_cache_cntlr(l1_cache_cntlr)
   , _dram_directory_home_lookup(dram_directory_home_lookup)
   , _user_thread_sem(user_thread_sem)
   , _network_thread_sem(network_thread_sem)
{
   _l2_cache = new Cache("L2",
         l2_cache_size, 
         l2_cache_associativity, 
         cache_line_size, 
         l2_cache_replacement_policy, 
         Cache::PR_L2_CACHE,
         l2_cache_access_delay,
         frequency,
         l2_cache_track_miss_types);
}

L2CacheCntlr::~L2CacheCntlr()
{
   delete _l2_cache;
}

void
L2CacheCntlr::getCacheLineInfo(IntPtr address, PrL2CacheLineInfo* l2_cache_line_info)
{
   _l2_cache->getCacheLineInfo(address, l2_cache_line_info);
}

void
L2CacheCntlr::setCacheLineInfo(IntPtr address, PrL2CacheLineInfo* l2_cache_line_info)
{
   _l2_cache->setCacheLineInfo(address, l2_cache_line_info);
}

void
L2CacheCntlr::invalidateCacheLine(IntPtr address)
{
   _l2_cache->invalidateCacheLine(address);
}

void
L2CacheCntlr::readCacheLine(IntPtr address, Byte* data_buf)
{
   _l2_cache->accessCacheLine(address, Cache::LOAD, data_buf, getCacheLineSize());
}

void
L2CacheCntlr::writeCacheLine(IntPtr address, UInt32 offset, Byte* data_buf, UInt32 data_length)
{
   _l2_cache->accessCacheLine(address + offset, Cache::STORE, data_buf, data_length);
}

void
L2CacheCntlr::insertCacheLine(IntPtr address, CacheState::CState cstate, Byte* fill_buf, MemComponent::component_t mem_component)
{
   // Construct meta-data info about l2 cache line
   PrL2CacheLineInfo l2_cache_line_info;
   l2_cache_line_info.setTag(_l2_cache->getTag(address));
   l2_cache_line_info.setCState(cstate);
   l2_cache_line_info.setCachedLoc(mem_component);

   // Evicted line information
   bool eviction;
   IntPtr evicted_address;
   PrL2CacheLineInfo evicted_cache_line_info;
   Byte writeback_buf[getCacheLineSize()];

   _l2_cache->insertCacheLine(address, &l2_cache_line_info, fill_buf,
                              &eviction, &evicted_address, &evicted_cache_line_info, writeback_buf);

   if (eviction)
   {
      LOG_PRINT("Eviction: address(0x%x)", evicted_address);
      invalidateCacheLineInL1(evicted_cache_line_info.getCachedLoc(), evicted_address);

      UInt32 home_node_id = getHome(evicted_address);
      bool eviction_msg_modeled = getMemoryManager()->isMissTypeModeled(Cache::CAPACITY_MISS);

      if (evicted_cache_line_info.getCState() == CacheState::MODIFIED)
      {
         // Send back the data also
         ShmemMsg msg(ShmemMsg::FLUSH_REP, MemComponent::L2_CACHE, MemComponent::DRAM_DIRECTORY, getTileId(), evicted_address,
                      writeback_buf, getCacheLineSize(), eviction_msg_modeled);
         getMemoryManager()->sendMsg(home_node_id, msg);
      }
      else
      {
         LOG_ASSERT_ERROR(evicted_cache_line_info.getCState() == CacheState::SHARED,
               "evicted_address(0x%x), cache state(%u), cached loc(%u)",
               evicted_address, evicted_cache_line_info.getCState(), evicted_cache_line_info.getCachedLoc());
         ShmemMsg msg(ShmemMsg::INV_REP, MemComponent::L2_CACHE, MemComponent::DRAM_DIRECTORY, getTileId(), evicted_address, eviction_msg_modeled);
         getMemoryManager()->sendMsg(home_node_id, msg);
      }
   }
}

void
L2CacheCntlr::setCacheLineStateInL1(MemComponent::component_t mem_component, IntPtr address, CacheState::CState cstate)
{
   if (mem_component != MemComponent::INVALID_MEM_COMPONENT)
      _l1_cache_cntlr->setCacheLineState(mem_component, address, cstate);
}

void
L2CacheCntlr::invalidateCacheLineInL1(MemComponent::component_t mem_component, IntPtr address)
{
   if (mem_component != MemComponent::INVALID_MEM_COMPONENT)
      _l1_cache_cntlr->invalidateCacheLine(mem_component, address);
}

void
L2CacheCntlr::insertCacheLineInL1(MemComponent::component_t mem_component, IntPtr address,
                                  CacheState::CState cstate, Byte* fill_buf)
{
   assert(mem_component != MemComponent::INVALID_MEM_COMPONENT);

   bool eviction;
   IntPtr evicted_address;

   // Insert the Cache Line in L1 Cache
   _l1_cache_cntlr->insertCacheLine(mem_component, address, cstate, fill_buf, &eviction, &evicted_address);

   if (eviction)
   {
      // Clear the Present bit in L2 Cache corresponding to the evicted line
      // Get the cache line info first
      PrL2CacheLineInfo evicted_cache_line_info;
      getCacheLineInfo(evicted_address, &evicted_cache_line_info);
      // Clear the present bit and store the info back
      evicted_cache_line_info.clearCachedLoc(mem_component);
      setCacheLineInfo(evicted_address, &evicted_cache_line_info);
   }
}

void
L2CacheCntlr::insertCacheLineInHierarchy(IntPtr address, CacheState::CState cstate, Byte* fill_buf)
{
   assert(address == _outstanding_shmem_msg.getAddress());
   MemComponent::component_t mem_component = _outstanding_shmem_msg.getSenderMemComponent();
  
   // Insert Line in the L2 cache
   insertCacheLine(address, cstate, fill_buf, mem_component);
   
   // Insert Line in the L1 cache
   insertCacheLineInL1(mem_component, address, cstate, fill_buf);
}

pair<bool,Cache::MissType>
L2CacheCntlr::processShmemRequestFromL1Cache(MemComponent::component_t mem_component, ShmemMsg::msg_t msg_type, IntPtr address, bool modeled)
{
   LOG_PRINT("processShmemRequestFromL1Cache[Mem Component(%u), Msg Type(%u), Address(%#llx)]",
             mem_component, msg_type, address);

   PrL2CacheLineInfo l2_cache_line_info;
   getCacheLineInfo(address, &l2_cache_line_info);

   // Get the state associated with the address in the L2 cache
   CacheState::CState cstate = l2_cache_line_info.getCState();

   // Return arguments is a pair
   // (1) Is cache miss? (2) Cache miss type (COLD, CAPACITY, UPGRADE, SHARING)
   pair<bool,Cache::MissType> shmem_request_status_in_l2_cache = shmemRequestStatusInL2Cache(msg_type, address, cstate, modeled);
   if (!shmem_request_status_in_l2_cache.first)
   {
      Byte data_buf[getCacheLineSize()];
      
      // Read the cache line from L2 cache
      readCacheLine(address, data_buf);

      // Insert the cache line in the L1 cache
      insertCacheLineInL1(mem_component, address, cstate, data_buf);
      
      // Set that the cache line in present in the L1 cache in the L2 tags
      l2_cache_line_info.setCachedLoc(mem_component);
      setCacheLineInfo(address, &l2_cache_line_info);
   }
   
   return shmem_request_status_in_l2_cache;
}

void
L2CacheCntlr::handleMsgFromL1Cache(ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
   ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();
   MemComponent::component_t sender_mem_component = shmem_msg->getSenderMemComponent();

   acquireLock();

   assert(shmem_msg->getDataBuf() == NULL);
   assert(shmem_msg->getDataLength() == 0);

   assert(_outstanding_shmem_msg.getAddress() == INVALID_ADDRESS);

   // Set outstanding shmem msg parameters
   _outstanding_shmem_msg.setAddress(address);
   _outstanding_shmem_msg.setSenderMemComponent(sender_mem_component);
   _outstanding_shmem_msg_time = getShmemPerfModel()->getCycleCount();
   
   switch (shmem_msg_type)
   {
      case ShmemMsg::EX_REQ:
         processExReqFromL1Cache(shmem_msg);
         break;

      case ShmemMsg::SH_REQ:
         processShReqFromL1Cache(shmem_msg);
         break;

      default:
         LOG_PRINT_ERROR("Unrecognized shmem msg type (%u)", shmem_msg_type);
         break;
   }

   releaseLock();
}

void
L2CacheCntlr::processExReqFromL1Cache(ShmemMsg* shmem_msg)
{
   // We need to send a request to the Dram Directory Cache
   IntPtr address = shmem_msg->getAddress();

   PrL2CacheLineInfo l2_cache_line_info;
   getCacheLineInfo(address, &l2_cache_line_info);
   CacheState::CState cstate = l2_cache_line_info.getCState();

   assert((cstate == CacheState::INVALID) || (cstate == CacheState::SHARED));
   if (cstate == CacheState::SHARED)
   {
      // This will clear the 'Present' bit also
      invalidateCacheLine(address);
      ShmemMsg msg(ShmemMsg::INV_REP, MemComponent::L2_CACHE, MemComponent::DRAM_DIRECTORY, getTileId(), address, shmem_msg->isModeled());
      getMemoryManager()->sendMsg(getHome(address), msg);
   }

   // Send out EX_REQ to DRAM_DIRECTORY
   ShmemMsg msg(ShmemMsg::EX_REQ, MemComponent::L2_CACHE, MemComponent::DRAM_DIRECTORY, getTileId(), address, shmem_msg->isModeled());
   getMemoryManager()->sendMsg(getHome(address), msg);
}

void
L2CacheCntlr::processShReqFromL1Cache(ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   // Send out SH_REQ ro DRAM_DIRECTORY
   ShmemMsg msg(ShmemMsg::SH_REQ, MemComponent::L2_CACHE, MemComponent::DRAM_DIRECTORY, getTileId(), address, shmem_msg->isModeled());
   getMemoryManager()->sendMsg(getHome(address), msg);
}

void
L2CacheCntlr::handleMsgFromDramDirectory(tile_id_t sender, ShmemMsg* shmem_msg)
{
   ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();
   IntPtr address = shmem_msg->getAddress();

   // Acquire Locks
   MemComponent::component_t caching_mem_component = acquireL1CacheLock(shmem_msg_type, address);
   acquireLock();

   switch (shmem_msg_type)
   {
      case ShmemMsg::EX_REP:
         processExRepFromDramDirectory(sender, shmem_msg);
         break;
      case ShmemMsg::SH_REP:
         processShRepFromDramDirectory(sender, shmem_msg);
         break;
      case ShmemMsg::INV_REQ:
         processInvReqFromDramDirectory(sender, shmem_msg);
         break;
      case ShmemMsg::FLUSH_REQ:
         processFlushReqFromDramDirectory(sender, shmem_msg);
         break;
      case ShmemMsg::WB_REQ:
         processWbReqFromDramDirectory(sender, shmem_msg);
         break;
      default:
         LOG_PRINT_ERROR("Unrecognized msg type: %u", shmem_msg_type);
         break;
   }

   // Release Locks
   releaseLock();
   if (caching_mem_component != MemComponent::INVALID_MEM_COMPONENT)
      _l1_cache_cntlr->releaseLock(caching_mem_component);

   if ((shmem_msg_type == ShmemMsg::EX_REP) || (shmem_msg_type == ShmemMsg::SH_REP))
   {
      assert(_outstanding_shmem_msg_time <= getShmemPerfModel()->getCycleCount());
      
      // Reset the clock to the time the request left the tile is miss type is not modeled
      if (!shmem_msg->isModeled())
         getShmemPerfModel()->setCycleCount(_outstanding_shmem_msg_time);

      // Increment the clock by the time taken to update the L2 cache
      getMemoryManager()->incrCycleCount(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);

      // Set the clock of the APP thread to that of the SIM thread
      getShmemPerfModel()->setCycleCount(ShmemPerfModel::_USER_THREAD, 
                                         getShmemPerfModel()->getCycleCount());
      
      // There are no more outstanding memory requests
      _outstanding_shmem_msg.setAddress(INVALID_ADDRESS);
      
      LOG_PRINT("wake up app thread");
      wakeUpUserThread();
      LOG_PRINT("wait for app thread");
      waitForUserThread();
      LOG_PRINT("done waiting");
   }
}

void
L2CacheCntlr::processExRepFromDramDirectory(tile_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
   Byte* data_buf = shmem_msg->getDataBuf();
   
   // Insert Cache Line in L1 and L2 Caches
   insertCacheLineInHierarchy(address, CacheState::MODIFIED, data_buf);
}

void
L2CacheCntlr::processShRepFromDramDirectory(tile_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
   Byte* data_buf = shmem_msg->getDataBuf();

   // Insert Cache Line in L1 and L2 Caches
   insertCacheLineInHierarchy(address, CacheState::SHARED, data_buf);
}

void
L2CacheCntlr::processInvReqFromDramDirectory(tile_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   PrL2CacheLineInfo l2_cache_line_info;
   getCacheLineInfo(address, &l2_cache_line_info);
   
   CacheState::CState cstate = l2_cache_line_info.getCState();

   if (cstate != CacheState::INVALID)
   {
      assert(cstate == CacheState::SHARED);
  
      // Update Shared memory performance counters for access to L2 Cache
      getMemoryManager()->incrCycleCount(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_TAGS);
      // Update Shared Mem perf counters for access to L1 Cache
      getMemoryManager()->incrCycleCount(l2_cache_line_info.getCachedLoc(), CachePerfModel::ACCESS_CACHE_TAGS);

      // Invalidate the line in L1 Cache
      invalidateCacheLineInL1(l2_cache_line_info.getCachedLoc(), address);
      // Invalidate the line in the L2 cache also
      invalidateCacheLine(address);

      // Send out INV_REP to DRAM_DIRECTORY
      ShmemMsg msg(ShmemMsg::INV_REP, MemComponent::L2_CACHE, MemComponent::DRAM_DIRECTORY, shmem_msg->getRequester(), address, shmem_msg->isModeled());
      getMemoryManager()->sendMsg(sender, msg);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrCycleCount(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_TAGS);
   }
}

void
L2CacheCntlr::processFlushReqFromDramDirectory(tile_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   PrL2CacheLineInfo l2_cache_line_info;
   getCacheLineInfo(address, &l2_cache_line_info);
   CacheState::CState cstate = l2_cache_line_info.getCState();
   if (cstate != CacheState::INVALID)
   {
      assert(cstate == CacheState::MODIFIED);
      
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrCycleCount(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);
      // Update Shared Mem perf counters for access to L1 Cache
      getMemoryManager()->incrCycleCount(l2_cache_line_info.getCachedLoc(), CachePerfModel::ACCESS_CACHE_TAGS);

      // Invalidate the line in L1 Cache
      invalidateCacheLineInL1(l2_cache_line_info.getCachedLoc(), address);

      // Write-back the line
      Byte data_buf[getCacheLineSize()];
      readCacheLine(address, data_buf);

      // Invalidate the line
      invalidateCacheLine(address);

      // Send FLUSH_REP to DRAM_DIRECTORY
      ShmemMsg msg(ShmemMsg::FLUSH_REP, MemComponent::L2_CACHE, MemComponent::DRAM_DIRECTORY, shmem_msg->getRequester(), address,
                   data_buf, getCacheLineSize(), shmem_msg->isModeled());
      getMemoryManager()->sendMsg(sender, msg);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrCycleCount(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_TAGS);
   }
}

void
L2CacheCntlr::processWbReqFromDramDirectory(tile_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   PrL2CacheLineInfo l2_cache_line_info;
   getCacheLineInfo(address, &l2_cache_line_info);
   CacheState::CState cstate = l2_cache_line_info.getCState();

   if (cstate != CacheState::INVALID)
   {
      assert(cstate == CacheState::MODIFIED);
 
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrCycleCount(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);
      // Update Shared Mem perf counters for access to L1 Cache
      getMemoryManager()->incrCycleCount(l2_cache_line_info.getCachedLoc(), CachePerfModel::ACCESS_CACHE_TAGS);

      // Set the Appropriate Cache State in L1 also
      setCacheLineStateInL1(l2_cache_line_info.getCachedLoc(), address, CacheState::SHARED);

      // Write-Back the line
      Byte data_buf[getCacheLineSize()];
      readCacheLine(address, data_buf);
      
      // Set the cache line state to SHARED
      l2_cache_line_info.setCState(CacheState::SHARED);
      setCacheLineInfo(address, &l2_cache_line_info);

      // Send WB_REP to DRAM_DIRECTORY
      ShmemMsg msg(ShmemMsg::WB_REP, MemComponent::L2_CACHE, MemComponent::DRAM_DIRECTORY, shmem_msg->getRequester(), address,
                   data_buf, getCacheLineSize(), shmem_msg->isModeled());
      getMemoryManager()->sendMsg(sender, msg);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrCycleCount(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_TAGS);
   }
}

pair<bool,Cache::MissType>
L2CacheCntlr::shmemRequestStatusInL2Cache(ShmemMsg::msg_t shmem_msg_type, IntPtr address, CacheState::CState cstate, bool modeled)
{
   bool cache_hit = false;

   switch (shmem_msg_type)
   {
   case ShmemMsg::EX_REQ:
      cache_hit = CacheState(cstate).writable();
      break;

   case ShmemMsg::SH_REQ:
      cache_hit = CacheState(cstate).readable();
      break;

   default:
      LOG_PRINT_ERROR("Unsupported Shmem Msg Type: %u", shmem_msg_type);
      break;
   }

   Cache::MissType cache_miss_type = Cache::INVALID_MISS_TYPE;
   if (modeled)
   {
      _l2_cache->updateMissCounters(address, !cache_hit);
   }

   return make_pair(!cache_hit, cache_miss_type);
}

MemComponent::component_t
L2CacheCntlr::acquireL1CacheLock(ShmemMsg::msg_t msg_type, IntPtr address)
{
   switch (msg_type)
   {
   case ShmemMsg::EX_REP:
   case ShmemMsg::SH_REP:
      
      assert(_outstanding_shmem_msg.getAddress() == address);
      assert(_outstanding_shmem_msg.getSenderMemComponent() != MemComponent::INVALID_MEM_COMPONENT);
      _l1_cache_cntlr->acquireLock(_outstanding_shmem_msg.getSenderMemComponent());
      return _outstanding_shmem_msg.getSenderMemComponent();

   case ShmemMsg::INV_REQ:
   case ShmemMsg::FLUSH_REQ:
   case ShmemMsg::WB_REQ:
   
      {
         acquireLock();
         
         PrL2CacheLineInfo l2_cache_line_info;
         getCacheLineInfo(address, &l2_cache_line_info);
         MemComponent::component_t caching_mem_component = l2_cache_line_info.getCachedLoc();
         
         releaseLock();

         assert(caching_mem_component != MemComponent::L1_ICACHE);
         if (caching_mem_component != MemComponent::INVALID_MEM_COMPONENT)
         {
            _l1_cache_cntlr->acquireLock(caching_mem_component);
         }
         return caching_mem_component;
      }

   default:
      LOG_PRINT_ERROR("Unrecognized Msg Type (%u)", msg_type);
      return MemComponent::INVALID_MEM_COMPONENT;
   }
}

void
L2CacheCntlr::acquireLock()
{
   _l2_cache_lock.acquire();
}

void
L2CacheCntlr::releaseLock()
{
   _l2_cache_lock.release();
}

void
L2CacheCntlr::wakeUpUserThread()
{
   _user_thread_sem->signal();
}

void
L2CacheCntlr::waitForUserThread()
{
   _network_thread_sem->wait();
}

tile_id_t
L2CacheCntlr::getTileId()
{
   return _memory_manager->getTile()->getId();
}

UInt32
L2CacheCntlr::getCacheLineSize()
{ 
   return _memory_manager->getCacheLineSize();
}
 
ShmemPerfModel*
L2CacheCntlr::getShmemPerfModel()
{ 
   return _memory_manager->getShmemPerfModel();
}

}
