// Copyright (c) 2009-2011, Tor M. Aamodt, Wilson W.L. Fung, Ali Bakhoda,
// Ivan Sham, George L. Yuan,
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

#include "gpu-sim.h"
#include "gpu-misc.h"
#include "dram.h"
#include "mem_latency_stat.h"
#include "dram_sched.h"
#include "mem_fetch.h"
#include "l2cache.h"

#ifdef DRAM_VERIFY
int PRINT_CYCLE = 0;
#endif

template class fifo_pipeline<mem_fetch>;
template class fifo_pipeline<dram_req_t>;

dram_t::dram_t( unsigned int partition_id, const struct memory_config *config, memory_stats_t *stats,
                memory_partition_unit *mp )
{
   id = partition_id;
   m_memory_partition_unit = mp;
   m_stats = stats;
   m_config = config;

   CCDc = 0;
   RRDc = 0;
   RTWc = 0;
   WTRc = 0;

   rw = READ; //read mode is default

	bkgrp = (bankgrp_t**) calloc(sizeof(bankgrp_t*), m_config->nbkgrp);
	bkgrp[0] = (bankgrp_t*) calloc(sizeof(bank_t), m_config->nbkgrp);
	for (unsigned i=1; i<m_config->nbkgrp; i++) {
		bkgrp[i] = bkgrp[0] + i;
	}
	for (unsigned i=0; i<m_config->nbkgrp; i++) {
		bkgrp[i]->CCDLc = 0;
		bkgrp[i]->RTPLc = 0;
	}

   bk = (bank_t**) calloc(sizeof(bank_t*),m_config->nbk);
   bk[0] = (bank_t*) calloc(sizeof(bank_t),m_config->nbk);
   for (unsigned i=1;i<m_config->nbk;i++) 
      bk[i] = bk[0] + i;
   for (unsigned i=0;i<m_config->nbk;i++) {
      bk[i]->state = BANK_IDLE;
      bk[i]->bkgrpindex = i/(m_config->nbk/m_config->nbkgrp);
   }
   prio = 0;  
   rwq = new fifo_pipeline<dram_req_t>("rwq",m_config->CL,m_config->CL+1);
   mrqq = new fifo_pipeline<dram_req_t>("mrqq",0,2);
   returnq = new fifo_pipeline<mem_fetch>("dramreturnq",0,m_config->gpgpu_dram_return_queue_size==0?1024:m_config->gpgpu_dram_return_queue_size); 
   m_frfcfs_scheduler = NULL;
   if ( m_config->scheduler_type == DRAM_FRFCFS )
      m_frfcfs_scheduler = new frfcfs_scheduler(m_config,this,stats);
   n_cmd = 0;
   n_activity = 0;
   n_nop = 0; 
   n_act = 0; 
   n_pre = 0; 
   n_rd = 0;
   n_wr = 0;
   n_req = 0;
   max_mrqs_temp = 0;
   bwutil = 0;
   max_mrqs = 0;
   ave_mrqs = 0;

   for (unsigned i=0;i<10;i++) {
      dram_util_bins[i]=0;
      dram_eff_bins[i]=0;
   }
   last_n_cmd = last_n_activity = last_bwutil = 0;

   n_cmd_partial = 0;
   n_activity_partial = 0;
   n_nop_partial = 0;  
   n_act_partial = 0;  
   n_pre_partial = 0;  
   n_req_partial = 0;
   ave_mrqs_partial = 0;
   bwutil_partial = 0;

   if ( queue_limit() )
      mrqq_Dist = StatCreate("mrqq_length",1, queue_limit());
   else //queue length is unlimited; 
      mrqq_Dist = StatCreate("mrqq_length",1,64); //track up to 64 entries
}

bool dram_t::full() const 
{
    if(m_config->scheduler_type == DRAM_FRFCFS ){
        if(m_config->gpgpu_frfcfs_dram_sched_queue_size == 0 ) return false;
        return m_frfcfs_scheduler->num_pending() >= m_config->gpgpu_frfcfs_dram_sched_queue_size;
    }
   else return mrqq->full();
}

unsigned dram_t::que_length() const
{
   unsigned nreqs = 0;
   if (m_config->scheduler_type == DRAM_FRFCFS ) {
      nreqs = m_frfcfs_scheduler->num_pending();
   } else {
      nreqs = mrqq->get_length();
   }
   return nreqs;
}

bool dram_t::returnq_full() const
{
   return returnq->full();
}

unsigned int dram_t::queue_limit() const 
{ 
   return m_config->gpgpu_frfcfs_dram_sched_queue_size; 
}


dram_req_t::dram_req_t( class mem_fetch *mf )
{
   txbytes = 0;   // 初始化为0，tx：texture？
   dqbytes = 0;   // 初始化为0，dq：
   data = mf;     // 数据，mem_fetch

   const addrdec_t &tlx = mf->get_tlx_addr();   // 原始物理地址，行-bank-列地址

   bk  = tlx.bk;     // bank地址？bank数量？
   row = tlx.row;    // 行地址，第几行
   col = tlx.col;    // 列地址，第几列
   nbytes = mf->get_data_size(); // 数据部分大小（不包括头部）

   timestamp = gpu_tot_sim_cycle + gpu_sim_cycle;  // 设置dram_req_t的创建时间
   addr = mf->get_addr();                 // 形如：0XABC44545EF，这样的地址
   insertion_time = (unsigned) gpu_sim_cycle;      // 在何时插入的
   rw = data->get_is_write()?WRITE:READ;  // 确定读/写操作
}

void dram_t::push( class mem_fetch *data )  // 将“DRAM延时队列”中的元素放入DRAM channel
{
   assert(id == data->get_tlx_addr().chip); // 判断请求（data）的地址是否正确（是不是这个DRAM channel） /// Ensure request is in correct memory partition

   dram_req_t *mrq = new dram_req_t(data);  // 根据data，生成DRAM channel的访存请求mrq（memory request）
   data->set_status(IN_PARTITION_MC_INTERFACE_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);  // 设置data的状态
   mrqq->push(mrq);  // 将访存请求mrq，放入FIFO pipeline

   // stats...统计信息
   n_req += 1;          // 总的请求数量+1
   n_req_partial += 1;  // 该DRAM channel上的请求数量+1
   if ( m_config->scheduler_type == DRAM_FRFCFS ) {   // 判断DRAM channel的调度方式
      unsigned nreqs = m_frfcfs_scheduler->num_pending();   // 当前请求的数量
      if ( nreqs > max_mrqs_temp)                     // 与之前的记录比较
         max_mrqs_temp = nreqs;                       // 更新最大请求数
   } else {    // FIFO调度方式
      max_mrqs_temp = (max_mrqs_temp > mrqq->get_length())? max_mrqs_temp : mrqq->get_length();    // 记录最大请求数
   }
   m_stats->memlatstat_dram_access(data); // 统计
}

void dram_t::scheduler_fifo()    // FIFO调度策略
{  // 通过FIFO调度，让访存请求进入DRAM channel指定的bank中
   if (!mrqq->empty()) {   // dram_req_t 类型队列，判断是否为空
      unsigned int bkn;    // bank的ID
      dram_req_t *head_mrqq = mrqq->top();   // 取出队首元素（dram_req_t）
      head_mrqq->data->set_status(IN_PARTITION_MC_BANK_ARB_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle); // 设置状态
      bkn = head_mrqq->bk; // 获取该内存访问的bank的ID
      if (!bk[bkn]->mrq)   // 查看这个bank上是否存在请求
         bk[bkn]->mrq = mrqq->pop();   // 不存在请求（请求是空的），则将FIFO中队首请求弹出，放入bank（此时认为：访存请求进入DRAM channel）
   }
}


#define DEC2ZERO(x) x = (x)? (x-1) : 0;
#define SWAP(a,b) a ^= b; b ^= a; a ^= b;

void dram_t::cycle()    // DRAM channel的运行函数，整个DRAM的运行，其实就是全部DRAM channel的运行
{
   // 查看DRAM channel的“DRAM返回队列”是否满了
   if( !returnq->full() ) {
      dram_req_t *cmd = rwq->pop();   // 从rwq中取出cmd（访存请求，可能是：读/写）
      if( cmd ) {   // cmd不空
#ifdef DRAM_VIEWCMD 
           printf("\tDQ: BK%d Row:%03x Col:%03x", cmd->bk, cmd->row, cmd->col + cmd->dqbytes);  // DQ: BK   Row:row  Col:col + dqbytes
#endif
         cmd->dqbytes += m_config->dram_atom_size;  // 应该是一次读/写命令，所能取出/写入的bytes
         if (cmd->dqbytes >= cmd->nbytes) {
            mem_fetch *data = cmd->data;      // 获取cmd的数据（mem_fetch）即：mf
            data->set_status(IN_PARTITION_MC_RETURNQ,gpu_sim_cycle+gpu_tot_sim_cycle);  // 修改mf（data）的状态
            if( data->get_access_type() != L1_WRBK_ACC && data->get_access_type() != L2_WRBK_ACC ) {  // 条件判断，不允许L1_WRBK_ACC、L2_WRBK_ACC访问类型进入
               data->set_reply();      // 更改data的类型
               returnq->push(data);    // 进入”DRAM返回队列“
            } else {
               m_memory_partition_unit->set_done(data);  // data处理结束（收尾操作）
               delete data;                              // 删除data
            }
            delete cmd;  // 删除访问请求cmd
         }
#ifdef DRAM_VIEWCMD 
           printf("\n");
#endif
      }
   }

   /* 检查：刚刚到来的请求中，有没有落在空闲bank上的  /// check if the upcoming request is on an idle bank */   
   /* 我们应该修改它以便检查多个请求吗？  /// Should we modify this so that multiple requests are checked? */

   switch (m_config->scheduler_type) { // 根据调度类型，选择调度器的运行函数
   case DRAM_FIFO: scheduler_fifo(); break;        // FIFO调度，先进先出
   case DRAM_FRFCFS: scheduler_frfcfs(); break;    // FRFCFS，先行的先来先服务
	default:
		printf("Error: Unknown DRAM scheduler type\n");
		assert(0);
   }  // 小结：switch的内容，执行调度
   if ( m_config->scheduler_type == DRAM_FRFCFS ) {   // 判断调度类型： FRFCFS
      unsigned nreqs = m_frfcfs_scheduler->num_pending();   // FRFCFS调度器当前的访存请求数量（m_frfcfs_scheduler->num_pending()）
      if ( nreqs > max_mrqs) {   // 判断 当前请求数量 是否大于 之前记录的最大请求数量
         max_mrqs = nreqs;       // 更新最大请求数量
      }
      ave_mrqs += nreqs;         // 计算总的访存请求数量
      ave_mrqs_partial += nreqs; // 计算该DRAM channel的总的访存请求数量
   } else { // FIFO调度
      if (mrqq->get_length() > max_mrqs) {   // 当前请求数量 = mrqq->get_length()
         max_mrqs = mrqq->get_length();      // 更新最大请求数量
      }
      ave_mrqs += mrqq->get_length();           // 计算总的访存请求数量
      ave_mrqs_partial +=  mrqq->get_length();  // 计算该DRAM channel的总的访存请求数量
   }

   unsigned k=m_config->nbk;  // bank数量
   bool issued = false;       // 发射标志位

   // 检查：是否存在bank，已经准备好了读操作 /// check if any bank is ready to issue a new read  
   for (unsigned i=0;i<m_config->nbk;i++) {  // 遍历bank
      unsigned j = (i + prio) % m_config->nbk;     // prio：上一次的bankID
	   unsigned grp = j>>m_config->bk_tag_length;   // 计算bank所在的group的ID
      if (bk[j]->mrq) { // bank[j]中存在访存请求mrq     /// if currently servicing a memory request
         bk[j]->mrq->data->set_status(IN_PARTITION_DRAM,gpu_sim_cycle+gpu_tot_sim_cycle);   // 设置该请求的data（mf）状态
         // *************************************** 访存请求mrq的行为：”读取“，bank[j]处于“激活”状态（上一次操作的”行选信号“仍有效 且 上次的“行选信号” == mrq的”行选信号“） /// correct row activated for a READ /// 这里说一说为什么CCDc、RCDc等相关的时延参数必须等于0
         if ( !issued && !CCDc && !bk[j]->RCDc &&  // 未发射 && CCDc：列选信号之间的开销 && bank[j]执行”读取“操作时，从“行选信号”选通到”列选信号“选通所花费的时间 == 0
              !(bkgrp[grp]->CCDLc) &&              // bank[j]所在的group的CCDLc == 0
              (bk[j]->curr_row == bk[j]->mrq->row) &&       // ID为j的bank（bank[j]）当前的行号 == 访存请求指定的行号
              (bk[j]->mrq->rw == READ) && (WTRc == 0 )  &&  // 访存请求mrq的操作为“读取” && ”写入“转为”读取“所花的时间 == 0
              (bk[j]->state == BANK_ACTIVE) &&     // ID为j的bank(bank[j])处于激活状态
              !rwq->full() ) {                     // rwq（FIFO pipeline，类似链表的数据结构）处于不满状态（还能放进来）
            if (rw==WRITE) {  // 查看上一次访存操作是否为写入
               rw=READ;       // 修改为读取（因为本次操作为：读取）
               rwq->set_min_length(m_config->CL);  // 设置rwq的最小长度（FIFO pipeline，FIFO流水线？）   新的最小长度 == CL（实际上就是读取的时延）
            }
            rwq->push(bk[j]->mrq);     // 将内存请求（mrq）放入rwq
            bk[j]->mrq->txbytes += m_config->dram_atom_size;   // + 每个“读”或“写”命令传输的字节数
            CCDc = m_config->tCCD;                             // 重置：CCDc。“列选信号”切换的开销（花费的时间）
            bkgrp[grp]->CCDLc = m_config->tCCDL;               // 重置：CCDLc。当bank的group有效时，从一个列选信号切换到另一个列选信号，所需要的时间 //column to column delay when bank groups are enabled 
            RTWc = m_config->tRTW;  // 重置：RTWc。内存从“读取”状态转换到“写入”状态，所需要的时间（适用于所有bank）  /// read to write penalty applies across banks
            bk[j]->RTPc = m_config->BL/m_config->data_command_freq_ratio;  // 重置：RTPc。同一bank中，从“读取”状态转为”预充电“状态的开销
            bkgrp[grp]->RTPLc = m_config->tRTPL;      // 重置：RTPLc。bank group中，”读取“状态（数据总线）转为“预充电”（命令总线？）状态的开销
            issued = true;    // 更改”发射“状态标志位（mrq已经发射出去）
            n_rd++;
            bwutil += m_config->BL/m_config->data_command_freq_ratio;   // （BL/data_command_freq_ratio）代表什么？bank的RTPc
            bwutil_partial += m_config->BL/m_config->data_command_freq_ratio;
            bk[j]->n_access++;   // ID为j的bank，访问次数+1
#ifdef DRAM_VERIFY
            PRINT_CYCLE=1;
            printf("\tRD  Bk:%d Row:%03x Col:%03x \n",
                   j, bk[j]->curr_row,
                   bk[j]->mrq->col + bk[j]->mrq->txbytes - m_config->dram_atom_size);
#endif            
            // transfer done  判断传输是否完成： txbytes >= nbytes ， 表示传输完成了
            if ( !(bk[j]->mrq->txbytes < bk[j]->mrq->nbytes) ) {
               bk[j]->mrq = NULL;   // 将ID为j的bank中的访存请求mrq设置为空
            }
         } else
            // *************************************** 访存请求mrq的行为：”写入“，bank[j]处于激活状态，上一次的”行选信号“ == mrq的”行选信号“ /// correct row activated for a WRITE
            if ( !issued && !CCDc && !bk[j]->RCDWRc &&      // 未发射 && CCDc == 0 && RCDWRc == 0
                 !(bkgrp[grp]->CCDLc) &&                    // bank group的CCDLc == 0
                 (bk[j]->curr_row == bk[j]->mrq->row)  &&   // 上次”行选信号“ == mrq(本次)”行选信号“（可以免去”行激活时间“）
                 (bk[j]->mrq->rw == WRITE) && (RTWc == 0 )  && // mrq的操作为：”写入“ && RTWc == 0
                 (bk[j]->state == BANK_ACTIVE) &&           // bank[j]处于激活状态
                 !rwq->full() ) {                           // rwq不满（FIFO pipeline，链表结构）
            if (rw==READ) {   // 判断上一次的DRAM channel的操作
               rw=WRITE;      // 更改为本次操作：”写入“
               rwq->set_min_length(m_config->WL);  // 设置rwq的最小长度。  新的最小长度 == 数据的写入时延
            }
            rwq->push(bk[j]->mrq);  // 访存请求mrq放入rwq（FIFO pipeline，链表）

            bk[j]->mrq->txbytes += m_config->dram_atom_size;   // 设置传输的数据大小（写入数据大小）
            CCDc = m_config->tCCD;     // 重置DRAM channel的CCDc
            bkgrp[grp]->CCDLc = m_config->tCCDL;   // 重置bank[j] 所在group的 tCCDL
            WTRc = m_config->tWTR;     // 重置DRAM channel的WTRc
            bk[j]->WTPc = m_config->tWTP;          // 重置bank[j]的WTPc
            issued = true;             // 访存请求发射完毕
            n_wr++;                    // 内存写入次数+1
            bwutil += m_config->BL/m_config->data_command_freq_ratio;         // 为什么要加上这个？”读取“状态转为”预充电“的开销（同一bank）
            bwutil_partial += m_config->BL/m_config->data_command_freq_ratio; //
#ifdef DRAM_VERIFY
            PRINT_CYCLE=1;
            printf("\tWR  Bk:%d Row:%03x Col:%03x \n",
                   j, bk[j]->curr_row, 
                   bk[j]->mrq->col + bk[j]->mrq->txbytes - m_config->dram_atom_size);
#endif  
            // transfer done 确认本次数据传输结束： 传输数据大小 >= mrq的”写入“数据大小
            if ( !(bk[j]->mrq->txbytes < bk[j]->mrq->nbytes) ) {
               bk[j]->mrq = NULL;   // 将bank[j]的访存请求置为NULL
            }
         }

         else
            // *************************************** bank[j]处于空闲状态 /// bank is idle /// 注：寻址分两步：（1）发出”bank的ID“与”行选信号“    （2）在“行选信号”就绪的情况下，再发”列选信号“完成寻址   
            if ( !issued && !RRDc &&            // 未发射 && ”行选信号“之间的切换时延 == 0 （可以理解为：完成了“行选信号”的切换）
                 (bk[j]->state == BANK_IDLE) && // bank[j]处于空闲状态（未激活）
                 !bk[j]->RPc && !bk[j]->RCc ) { // 行预充电时间 == 0（预充电完成） && 行循环时间 == 0 （理解一下，就是”行选信号“已经就绪了）
#ifdef DRAM_VERIFY
            PRINT_CYCLE=1;
            printf("\tACT BK:%d NewRow:%03x From:%03x \n",
                   j,bk[j]->mrq->row,bk[j]->curr_row);
#endif
            // 本次操作完成寻址的第一步   /// activate the row with current memory request 
            bk[j]->curr_row = bk[j]->mrq->row;  // 给出bank[j]的“激活行”
            bk[j]->state = BANK_ACTIVE;         // 更改bank[j]的状态
            RRDc = m_config->tRRD;              // 重置：RRDc
            bk[j]->RCDc = m_config->tRCD;       // 重置：RCDc
            bk[j]->RCDWRc = m_config->tRCDWR;   // 重置：RCDWRc
            bk[j]->RASc = m_config->tRAS;       // 重置：RASc
            bk[j]->RCc = m_config->tRC;         // 重置：RCc
            prio = (j + 1) % m_config->nbk;     // 更新：上次操作的bank的ID
            issued = true;       // 更新发射状态
            n_act_partial++;     // 统计： DRAM channel的动作+1
            n_act++;             // 全局统计：DRAM的动作+1
         }

         else  // 下面的操作，有点没看懂，没有想通，仅仅是行不一样，为什么要关闭bank？这么一来“读取”/”写入“操作需要3个DRAM channel的cycle
            // *************************************** 虽然bank[j]处于激活状态，但是bank[j]中的“激活行” != mrq的”行选信号“ /// different row activated ///  关闭bank
            if ( (!issued) &&    // 未发射
                 (bk[j]->curr_row != bk[j]->mrq->row) && // bank[j]中的“激活行” != mrq的”行选信号“ 
                 (bk[j]->state == BANK_ACTIVE) &&        // bank[j]处于激活状态
                 (!bk[j]->RASc && !bk[j]->WTPc &&        // 关闭（猜测：关闭行 与 激活行 的开销一样）已经处于激活状态的bank中的行的时延 == 0 && ”写入“操作预充电完成
				      !bk[j]->RTPc &&                        // “读取”操作的预充电完成
				      !bkgrp[grp]->RTPLc) ) {                // bank group的”读取“预充电完成
            // make the bank idle again
            bk[j]->state = BANK_IDLE;        // 设置bank[j]的状态为：空闲
            bk[j]->RPc = m_config->tRP;      // 重置bank的行预充电时间
            prio = (j + 1) % m_config->nbk;  // 更新：上一次操作的bank的ID
            issued = true;    // 更改发射标志位
            n_pre++;          // 统计： 预处理+1
            n_pre_partial++;  // 全局统计： 预处理+1
#ifdef DRAM_VERIFY
            PRINT_CYCLE=1;
            printf("\tPRE BK:%d Row:%03x \n", j,bk[j]->curr_row);
#endif
         }  // 回顾之前的四个分支： 1、执行mrq的”读取“操作  2、执行mrq的”写入“操作  3、mrq所在的bank处于空闲状态  4、mrq所在的bank的“激活行”不对，需要关闭bank（变为：空闲）
      } else {    // mrq是空的
         if (!CCDc && !RRDc && !RTWc && !WTRc && !bk[j]->RCDc && !bk[j]->RASc
             && !bk[j]->RCc && !bk[j]->RPc  && !bk[j]->RCDWRc) k--;
         bk[j]->n_idle++;  // 空闲的bank数量+1
      }
   }
   if (!issued) {
      n_nop++;          // 计算DRAM的Nop的数量
      n_nop_partial++;  // 计算DRAM channel的Nop的数量
#ifdef DRAM_VIEWCMD
      printf("\tNOP                        ");
#endif
   }
   if (k) {
      n_activity++;           // 记录DRAM channel中非空的mrq的数量
      n_activity_partial++;   // 记录DRAM channel活跃的bank数量
   }
   n_cmd++;
   n_cmd_partial++;

   // 每过一个DRAM channel的cycle，DRAM channel的以下时延-1（如果是0，就不用减）   /// decrements counters once for each time dram_issueCMD is called
   DEC2ZERO(RRDc);   // RRDc--：不同的bank之间，”行选信号“之间的切换时延
   DEC2ZERO(CCDc);   // CCDc--：同一bank内，”列选信号“之间的切换时延
   DEC2ZERO(RTWc);   // RTWc--：DRAM channel的”读取“状态切换至”写入“状态的时延（惩罚）
   DEC2ZERO(WTRc);   // WTRc--：DRAM channel的”写入“状态切换至”读取“状态的时延（惩罚）
   for (unsigned j=0;j<m_config->nbk;j++) {        // 每过一个DRAM channel的cycle，所有bank的以下时延-1（如果是0，就不用减）
      DEC2ZERO(bk[j]->RCDc);  // RCDc--：”列选信号“发出到所选列处于”激活“状态的时延（访存请求：”读取“）
      DEC2ZERO(bk[j]->RASc);  // RASc--：”行选信号“发出到所选行处于”激活“状态的时延
      DEC2ZERO(bk[j]->RCc);   // RCc--：关闭当前的”激活行“之后，再去激活“其他行”所需要的时间 （再去激活其他行的时间）
      DEC2ZERO(bk[j]->RPc);   // RPc--：关闭当前的”激活行“所需要的时间
      DEC2ZERO(bk[j]->RCDWRc);// RCDWRc：”列选信号“发出到所选列处于”激活“状态的时延（访存请求：”写入“）
      DEC2ZERO(bk[j]->WTPc);  // WTPc--：bank从”写入“状态切换至”预充电“状态
      DEC2ZERO(bk[j]->RTPc);  // RTPc--：bank从”读取“状态切换至”预充电“状态
   }
   for (unsigned j=0; j<m_config->nbkgrp; j++) {   // 每过一个DRAM channel的cycle，所有bank group的以下时延-1（如果是0，就不用减）
	   DEC2ZERO(bkgrp[j]->CCDLc); // CCDLc--：当bank group使能有效时，”列选信号“之间切换的时延
	   DEC2ZERO(bkgrp[j]->RTPLc); // RTPLc--：当bank group使能有效时，从”读取“状态切换至”预充电“状态的时延
   }

#ifdef DRAM_VISUALIZE
   visualize();
#endif
}

//if mrq is being serviced by dram, gets popped after CL latency fulfilled
class mem_fetch* dram_t::return_queue_pop() 
{
    return returnq->pop();
}

class mem_fetch* dram_t::return_queue_top() 
{
    return returnq->top();
}

void dram_t::print( FILE* simFile) const
{
   unsigned i;
   fprintf(simFile,"DRAM[%d]: %d bks, busW=%d BL=%d CL=%d, ", 
           id, m_config->nbk, m_config->busW, m_config->BL, m_config->CL );
   fprintf(simFile,"tRRD=%d tCCD=%d, tRCD=%d tRAS=%d tRP=%d tRC=%d\n",
           m_config->tCCD, m_config->tRRD, m_config->tRCD, m_config->tRAS, m_config->tRP, m_config->tRC );
   fprintf(simFile,"n_cmd=%d n_nop=%d n_act=%d n_pre=%d n_req=%d n_rd=%d n_write=%d bw_util=%.4g\n",
           n_cmd, n_nop, n_act, n_pre, n_req, n_rd, n_wr,
           (float)bwutil/n_cmd);
   fprintf(simFile,"n_activity=%d dram_eff=%.4g\n",
           n_activity, (float)bwutil/n_activity);
   for (i=0;i<m_config->nbk;i++) {
      fprintf(simFile, "bk%d: %da %di ",i,bk[i]->n_access,bk[i]->n_idle);
   }
   fprintf(simFile, "\n");
   fprintf(simFile, "dram_util_bins:");
   for (i=0;i<10;i++) fprintf(simFile, " %d", dram_util_bins[i]);
   fprintf(simFile, "\ndram_eff_bins:");
   for (i=0;i<10;i++) fprintf(simFile, " %d", dram_eff_bins[i]);
   fprintf(simFile, "\n");
   if(m_config->scheduler_type== DRAM_FRFCFS) 
       fprintf(simFile, "mrqq: max=%d avg=%g\n", max_mrqs, (float)ave_mrqs/n_cmd);
}

void dram_t::visualize() const
{
   printf("RRDc=%d CCDc=%d mrqq.Length=%d rwq.Length=%d\n", 
          RRDc, CCDc, mrqq->get_length(),rwq->get_length());
   for (unsigned i=0;i<m_config->nbk;i++) {
      printf("BK%d: state=%c curr_row=%03x, %2d %2d %2d %2d %p ", 
             i, bk[i]->state, bk[i]->curr_row,
             bk[i]->RCDc, bk[i]->RASc,
             bk[i]->RPc, bk[i]->RCc,
             bk[i]->mrq );
      if (bk[i]->mrq)
         printf("txf: %d %d", bk[i]->mrq->nbytes, bk[i]->mrq->txbytes);
      printf("\n");
   }
   if ( m_frfcfs_scheduler ) 
      m_frfcfs_scheduler->print(stdout);
}

void dram_t::print_stat( FILE* simFile ) 
{
   fprintf(simFile,"DRAM (%d): n_cmd=%d n_nop=%d n_act=%d n_pre=%d n_req=%d n_rd=%d n_write=%d bw_util=%.4g ",
           id, n_cmd, n_nop, n_act, n_pre, n_req, n_rd, n_wr,
           (float)bwutil/n_cmd);
   fprintf(simFile, "mrqq: %d %.4g mrqsmax=%d ", max_mrqs, (float)ave_mrqs/n_cmd, max_mrqs_temp);
   fprintf(simFile, "\n");
   fprintf(simFile, "dram_util_bins:");
   for (unsigned i=0;i<10;i++) fprintf(simFile, " %d", dram_util_bins[i]);
   fprintf(simFile, "\ndram_eff_bins:");
   for (unsigned i=0;i<10;i++) fprintf(simFile, " %d", dram_eff_bins[i]);
   fprintf(simFile, "\n");
   max_mrqs_temp = 0;
}

void dram_t::visualizer_print( gzFile visualizer_file )
{
   // dram specific statistics
   gzprintf(visualizer_file,"dramncmd: %u %u\n",id, n_cmd_partial);  
   gzprintf(visualizer_file,"dramnop: %u %u\n",id,n_nop_partial);
   gzprintf(visualizer_file,"dramnact: %u %u\n",id,n_act_partial);
   gzprintf(visualizer_file,"dramnpre: %u %u\n",id,n_pre_partial);
   gzprintf(visualizer_file,"dramnreq: %u %u\n",id,n_req_partial);
   gzprintf(visualizer_file,"dramavemrqs: %u %u\n",id,
            n_cmd_partial?(ave_mrqs_partial/n_cmd_partial ):0);

   // utilization and efficiency
   gzprintf(visualizer_file,"dramutil: %u %u\n",  
            id,n_cmd_partial?100*bwutil_partial/n_cmd_partial:0);
   gzprintf(visualizer_file,"drameff: %u %u\n", 
            id,n_activity_partial?100*bwutil_partial/n_activity_partial:0);

   // reset for next interval
   bwutil_partial = 0;
   n_activity_partial = 0;
   ave_mrqs_partial = 0; 
   n_cmd_partial = 0;
   n_nop_partial = 0;
   n_act_partial = 0;
   n_pre_partial = 0;
   n_req_partial = 0;

   // dram access type classification
   for (unsigned j = 0; j < m_config->nbk; j++) {
      gzprintf(visualizer_file,"dramglobal_acc_r: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[GLOBAL_ACC_R][id][j]);
      gzprintf(visualizer_file,"dramglobal_acc_w: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[GLOBAL_ACC_W][id][j]);
      gzprintf(visualizer_file,"dramlocal_acc_r: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[LOCAL_ACC_R][id][j]);
      gzprintf(visualizer_file,"dramlocal_acc_w: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[LOCAL_ACC_W][id][j]);
      gzprintf(visualizer_file,"dramconst_acc_r: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[CONST_ACC_R][id][j]);
      gzprintf(visualizer_file,"dramtexture_acc_r: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[TEXTURE_ACC_R][id][j]);
   }
}


void dram_t::set_dram_power_stats(	unsigned &cmd,
									unsigned &activity,
									unsigned &nop,
									unsigned &act,
									unsigned &pre,
									unsigned &rd,
									unsigned &wr,
									unsigned &req) const{

	// 将电源性能计数器指向低级DRAM计数器  /// Point power performance counters to low-level DRAM counters
	cmd = n_cmd;
	activity = n_activity;
	nop = n_nop;
	act = n_act;
	pre = n_pre;
	rd = n_rd;
	wr = n_wr;
	req = n_req;
}
