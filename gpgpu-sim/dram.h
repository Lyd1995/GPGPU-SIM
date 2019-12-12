// Copyright (c) 2009-2011, Tor M. Aamodt, Ivan Sham, Ali Bakhoda, 
// George L. Yuan, Wilson W.L. Fung
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

#ifndef DRAM_H
#define DRAM_H

#include "delayqueue.h"
#include <set>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>

#define READ 'R'  //define read and write states
#define WRITE 'W'
#define BANK_IDLE 'I'
#define BANK_ACTIVE 'A'

class dram_req_t {
public:
   dram_req_t( class mem_fetch *data );

   unsigned int row;    // 需要激活的行地址
   unsigned int col;    // 需要激活的列地址
   unsigned int bk;     // bank的ID，猜想：DRAM channel的寻址是三维的
   unsigned int nbytes;    // 读取/写入的数据大小
   unsigned int txbytes;   // t：transferred    x：读取/写入
   unsigned int dqbytes;
   unsigned int age;
   unsigned int timestamp;
   unsigned char rw;    // 判断读/写操作  /// is the request a read or a write? 
   unsigned long long int addr;  // 访问（读/写）地址
   unsigned int insertion_time;  // 插入时间
   class mem_fetch * data;
};

struct bankgrp_t
{
	unsigned int CCDLc;  // 当bank的group有效时，从一个列选信号切换到另一个列选信号，所需要的时间 //column to column delay when bank groups are enabled 
	unsigned int RTPLc;  // “读取”操作的预充电时延                                         //read to precharge delay when bank groups are enabled for GDDR5 this is identical to RTPS, if for other DRAM this is different, you will need to split them in two
};

struct bank_t
{
   unsigned int RCDc;   // 执行“读取”操作时，从“行选信号”处于激活状态开始，到“列选信号”处于激活状态，所需要的时间（注：“行选信号”与“bank的ID”先行传输，等“行选信号”选中的行处于激活状态之后，才发出“列选信号”） // row to column delay - time required to activate a row before a read
   unsigned int RCDWRc; // 执行“写入”操作时，从“行选信号”处于激活状态开始，到“列选信号”处于激活状态，所需要的时间 //row to column delay for a write command
   unsigned int RASc;   // 激活“行选信号”所选中的行，花费的时间     //time needed to activate row
   unsigned int RPc;    // 行预充电时间（即：关闭“行选信号”选中的行所需要的时间），这里默认是在同一bank内的行之间的切换（L-Bank关闭现有工作行，准备打开新行的操作就是预充电（Precharge）。） // row precharge ie. deactivate row
   unsigned int RCc;    // 行循环时间（即：关闭之后，再激活不同行，所花费的时间） // row cycle time ie. precharge current, then activate different row
   unsigned int WTPc; // write to precharge  //time to switch from write to precharge in the same bank
   unsigned int RTPc; // read to precharge   //time to switch from read to precharge in the same bank

   unsigned char rw;    // bank的读写状态 （读取or写入）   /// is the bank reading or writing?   
   unsigned char state; // bank的执行状态（活跃or空闲）    /// is the bank active or idle?
   unsigned int curr_row;  // bank当前选中的”行“

   dram_req_t *mrq;        // 该bank上的访存请求（mrq）

   unsigned int n_access;
   unsigned int n_writes;
   unsigned int n_idle;

   unsigned int bkgrpindex;
};

struct mem_fetch;

class dram_t 
{
public:
   dram_t( unsigned int parition_id, const struct memory_config *config, class memory_stats_t *stats, 
           class memory_partition_unit *mp );

   bool full() const;
   void print( FILE* simFile ) const;
   void visualize() const;
   void print_stat( FILE* simFile );
   unsigned que_length() const; 
   bool returnq_full() const;
   unsigned int queue_limit() const;
   void visualizer_print( gzFile visualizer_file );

   class mem_fetch* return_queue_pop();
   class mem_fetch* return_queue_top();
   void push( class mem_fetch *data );
   void cycle();
   void dram_log (int task);

   class memory_partition_unit *m_memory_partition_unit;
   unsigned int id;

   // Power Model
   void set_dram_power_stats(unsigned &cmd,
								unsigned &activity,
								unsigned &nop,
								unsigned &act,
								unsigned &pre,
								unsigned &rd,
								unsigned &wr,
								unsigned &req) const;

private:
   void scheduler_fifo();
   void scheduler_frfcfs();

   const struct memory_config *m_config;

   bankgrp_t **bkgrp;

   bank_t **bk;
   unsigned int prio;

   unsigned int RRDc;   // 不同的bank之间，切换“行选信号”所花费的最短时间   //minimal time required between activation of rows in different banks
   unsigned int CCDc;   // ”列选信号“之间的切换开销 //column to column delay
   unsigned int RTWc;   // 内存从“读取”状态转换到“写入”状态，所需要的时间（适用于所有bank）  /// read to write penalty applies across banks
   unsigned int WTRc;   // 内存从“写入”状态转换到“读取”状态，所需要的时间（适用于所有bank）  /// write to read penalty applies across banks

   unsigned char rw; // 记录最后一次的访存请求（是“读取”or“写入”）   /// was last request a read or write? (important for RTW, WTR)

   unsigned int pending_writes;

   fifo_pipeline<dram_req_t> *rwq;  // 
   fifo_pipeline<dram_req_t> *mrqq; // 
   // 用来缓存DRAM处理结束时的数据包                                         /// buffer to hold packets when DRAM processing is over 
   // 在DRAM的时钟域，将数据包放入缓存，并且在L2 cache/ICNT的时钟域，弹出数据包   /// should be filled with dram clock and popped with l2 or icnt clock 
   fifo_pipeline<mem_fetch> *returnq;  // 用来缓存DRAM处理结束时的数据包。在DRAM的时钟域，将数据包放入缓存，并且在L2 cache/ICNT的时钟域，弹出数据包

   unsigned int dram_util_bins[10];
   unsigned int dram_eff_bins[10];
   unsigned int last_n_cmd, last_n_activity, last_bwutil;

   unsigned int n_cmd;
   unsigned int n_activity;
   unsigned int n_nop;
   unsigned int n_act;
   unsigned int n_pre;
   unsigned int n_rd;
   unsigned int n_wr;
   unsigned int n_req;
   unsigned int max_mrqs_temp;

   unsigned int bwutil;
   unsigned int max_mrqs;
   unsigned int ave_mrqs;

   class frfcfs_scheduler* m_frfcfs_scheduler;

   unsigned int n_cmd_partial;
   unsigned int n_activity_partial;
   unsigned int n_nop_partial; 
   unsigned int n_act_partial; 
   unsigned int n_pre_partial; 
   unsigned int n_req_partial;
   unsigned int ave_mrqs_partial;
   unsigned int bwutil_partial;

   struct memory_stats_t *m_stats;
   class Stats* mrqq_Dist; //memory request queue inside DRAM  

   friend class frfcfs_scheduler;
};

#endif /*DRAM_H*/
