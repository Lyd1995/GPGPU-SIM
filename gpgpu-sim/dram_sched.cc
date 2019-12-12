// Copyright (c) 2009-2011, Tor M. Aamodt, Ali Bakhoda, George L. Yuan,
// The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice, this
// list of conditions and the following disclaimer in the documentation and/or
// other materials provided with the distribution.
// Neither the name of The University of British Columbia nor the names of its
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "../abstract_hardware_model.h"
#include "mem_latency_stat.h"

frfcfs_scheduler::frfcfs_scheduler( const memory_config *config, dram_t *dm, memory_stats_t *stats )
{
   m_config = config;
   m_stats = stats;
   m_num_pending = 0;   // 初始化为0
   m_dram = dm;
   m_queue = new std::list<dram_req_t*>[m_config->nbk];  // 根据bank的数量，生成list数组，保存dram_req_t
   m_bins = new std::map<unsigned, std::list<std::list<dram_req_t*>::iterator> >[ m_config->nbk ]; // 映射，根据bank的ID号，映射到一个存放dram_req_t的二维数组，第一维：访问的行号
   m_last_row = new std::list<std::list<dram_req_t*>::iterator>*[ m_config->nbk ];  // 根据bank的数量生成二维list
   curr_row_service_time = new unsigned[m_config->nbk];  // 根据bank的数量生成数组，统计每个bank，当前行的服务时间
   row_service_timestamp = new unsigned[m_config->nbk];  // 根据bank的数量生成数组，统计每个bank，当前行的开始服务时间
   for ( unsigned i=0; i < m_config->nbk; i++ ) {
      m_queue[i].clear();
      m_bins[i].clear();
      m_last_row[i] = NULL;
      curr_row_service_time[i] = 0;
      row_service_timestamp[i] = 0;
   }

}

void frfcfs_scheduler::add_req( dram_req_t *req )  // 增加访存请求dram_req_t
{
   m_num_pending++;  // 记录访存数量
   m_queue[req->bk].push_front(req);   // list，根据bank编号(req->bk)，按照先来先服务顺序，存放请求req
   std::list<dram_req_t*>::iterator ptr = m_queue[req->bk].begin();  // 取出req->bk列表的第一个元素（请求）
   m_bins[req->bk][req->row].push_front( ptr ); // 将最新的请求放入 //newest reqs to the front
}

void frfcfs_scheduler::data_collection(unsigned int bank)
{
   if (gpu_sim_cycle > row_service_timestamp[bank]) {
      curr_row_service_time[bank] = gpu_sim_cycle - row_service_timestamp[bank];
      if (curr_row_service_time[bank] > m_stats->max_servicetime2samerow[m_dram->id][bank])
         m_stats->max_servicetime2samerow[m_dram->id][bank] = curr_row_service_time[bank];
   }
   curr_row_service_time[bank] = 0;
   row_service_timestamp[bank] = gpu_sim_cycle;
   if (m_stats->concurrent_row_access[m_dram->id][bank] > m_stats->max_conc_access2samerow[m_dram->id][bank]) {
      m_stats->max_conc_access2samerow[m_dram->id][bank] = m_stats->concurrent_row_access[m_dram->id][bank];
   }
   m_stats->concurrent_row_access[m_dram->id][bank] = 0;
   m_stats->num_activates[m_dram->id][bank]++;
}

dram_req_t *frfcfs_scheduler::schedule( unsigned bank, unsigned curr_row ) // FRFCFS的调度算法
{
   if ( m_last_row[bank] == NULL ) {   // 查看上一次（现在还处于激活状态）的行，其bank地址，为空，表示
      if ( m_queue[bank].empty() )     // 查看
         return NULL;

      std::map<unsigned,std::list<std::list<dram_req_t*>::iterator> >::iterator bin_ptr = m_bins[bank].find( curr_row );
      if ( bin_ptr == m_bins[bank].end()) {
         dram_req_t *req = m_queue[bank].back();
         bin_ptr = m_bins[bank].find( req->row );
         assert( bin_ptr != m_bins[bank].end() ); // where did the request go???
         m_last_row[bank] = &(bin_ptr->second);
         data_collection(bank);
      } else {
         m_last_row[bank] = &(bin_ptr->second);

      }
   }
   std::list<dram_req_t*>::iterator next = m_last_row[bank]->back();
   dram_req_t *req = (*next);

   m_stats->concurrent_row_access[m_dram->id][bank]++;
   m_stats->row_access[m_dram->id][bank]++;
   m_last_row[bank]->pop_back();

   m_queue[bank].erase(next);
   if ( m_last_row[bank]->empty() ) {
      m_bins[bank].erase( req->row );
      m_last_row[bank] = NULL;
   }
#ifdef DEBUG_FAST_IDEAL_SCHED
   if ( req )
      printf("%08u : DRAM(%u) scheduling memory request to bank=%u, row=%u\n", 
             (unsigned)gpu_sim_cycle, m_dram->id, req->bk, req->row );
#endif
   assert( req != NULL && m_num_pending != 0 ); 
   m_num_pending--;

   return req;
}


void frfcfs_scheduler::print( FILE *fp )
{
   for ( unsigned b=0; b < m_config->nbk; b++ ) {
      printf(" %u: queue length = %u\n", b, (unsigned)m_queue[b].size() );
   }
}

void dram_t::scheduler_frfcfs()  // 行优先的先来先服务
{  // 
   unsigned mrq_latency;   // 记录
   frfcfs_scheduler *sched = m_frfcfs_scheduler;   // FRFCFS调度器
   while ( !mrqq->empty() && (!m_config->gpgpu_frfcfs_dram_sched_queue_size || sched->num_pending() < m_config->gpgpu_frfcfs_dram_sched_queue_size)) {
      dram_req_t *req = mrqq->pop();   // 取出mrqq队首的请求 req（dram_req_t）
      // 循环条件：mrqq队列不空 && （ gpgpu_frfcfs_dram_sched_queue_size != 0   ||   FRFCFS调度器中的访存请求数量 < gpgpu_frfcfs_dram_sched_queue_size ）
      // Power stats
      // if(req->data->get_type() != READ_REPLY && req->data->get_type() != WRITE_ACK)
      m_stats->total_n_access++; // 更新统计量： 总的内存访问数量+1
      // 判断请求的类型
      if(req->data->get_type() == WRITE_REQUEST){        // 写入请求
    	  m_stats->total_n_writes++;
      } else if(req->data->get_type() == READ_REQUEST){  // 读取请求
    	  m_stats->total_n_reads++;
      }

      req->data->set_status(IN_PARTITION_MC_INPUT_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle); // 设置访问状态
      sched->add_req(req);    // 调度器中，加入该请求req
   }

   dram_req_t *req;
   unsigned i;
   for ( i=0; i < m_config->nbk; i++ ) {
      unsigned b = (i+prio)%m_config->nbk;
      if ( !bk[b]->mrq ) {

         req = sched->schedule(b, bk[b]->curr_row);

         if ( req ) {
            req->data->set_status(IN_PARTITION_MC_BANK_ARB_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);
            prio = (prio+1)%m_config->nbk;
            bk[b]->mrq = req;
            if (m_config->gpgpu_memlatency_stat) {
               mrq_latency = gpu_sim_cycle + gpu_tot_sim_cycle - bk[b]->mrq->timestamp;
               bk[b]->mrq->timestamp = gpu_tot_sim_cycle + gpu_sim_cycle;
               m_stats->mrq_lat_table[LOGB2(mrq_latency)]++;
               if (mrq_latency > m_stats->max_mrq_latency) {
                  m_stats->max_mrq_latency = mrq_latency;
               }
            }

            break;
         }
      }
   }
}
