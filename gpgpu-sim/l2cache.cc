// Copyright (c) 2009-2011, Tor M. Aamodt
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <list>
#include <set>

#include "../option_parser.h"
#include "mem_fetch.h"
#include "dram.h"
#include "gpu-cache.h"
#include "histogram.h"
#include "l2cache.h"
#include "../statwrapper.h"
#include "../abstract_hardware_model.h"
#include "gpu-sim.h"
#include "shader.h"
#include "mem_latency_stat.h"
#include "l2cache_trace.h"


mem_fetch * partition_mf_allocator::alloc(new_addr_type addr, mem_access_type type, unsigned size, bool wr ) const 
{
    assert( wr );
    mem_access_t access( type, addr, size, wr );
    mem_fetch *mf = new mem_fetch( access, 
                                   NULL,
                                   WRITE_PACKET_SIZE, 
                                   -1, 
                                   -1, 
                                   -1,
                                   m_memory_config );
    return mf;
}

memory_partition_unit::memory_partition_unit( unsigned partition_id, 
                                              const struct memory_config *config,
                                              class memory_stats_t *stats )
: m_id(partition_id), m_config(config), m_stats(stats), m_arbitration_metadata(config) 
{
    m_dram = new dram_t(m_id,m_config,m_stats,this);

    m_sub_partition = new memory_sub_partition*[m_config->m_n_sub_partition_per_memory_channel]; 
    for (unsigned p = 0; p < m_config->m_n_sub_partition_per_memory_channel; p++) {
        unsigned sub_partition_id = m_id * m_config->m_n_sub_partition_per_memory_channel + p; 
        m_sub_partition[p] = new memory_sub_partition(sub_partition_id, m_config, stats); 
    }
}

memory_partition_unit::~memory_partition_unit() 
{
    delete m_dram; 
    for (unsigned p = 0; p < m_config->m_n_sub_partition_per_memory_channel; p++) {
        delete m_sub_partition[p]; 
    } 
    delete[] m_sub_partition; 
}

memory_partition_unit::arbitration_metadata::arbitration_metadata(const struct memory_config *config) 
: m_last_borrower(config->m_n_sub_partition_per_memory_channel - 1), 
  m_private_credit(config->m_n_sub_partition_per_memory_channel, 0), 
  m_shared_credit(0) 
{
    // each sub partition get at least 1 credit for forward progress 
    // the rest is shared among with other partitions 
    m_private_credit_limit = 1; 
    m_shared_credit_limit = config->gpgpu_frfcfs_dram_sched_queue_size 
                            + config->gpgpu_dram_return_queue_size 
                            - (config->m_n_sub_partition_per_memory_channel - 1); 
    if (config->gpgpu_frfcfs_dram_sched_queue_size == 0 
        or config->gpgpu_dram_return_queue_size == 0) 
    {
        m_shared_credit_limit = 0; // no limit if either of the queue has no limit in size 
    }
    assert(m_shared_credit_limit >= 0); 
}

bool memory_partition_unit::arbitration_metadata::has_credits(int inner_sub_partition_id) const     // 判断传入子分区（L2 cache）是否有信用
{   // 子分区的信用 小于 信用限制 返回 true
    int spid = inner_sub_partition_id;  // L2 cache（子分区）的本地ID
    if (m_private_credit[spid] < m_private_credit_limit) {  // 子分区的私人信用 < 私人信用限制
        return true;    // 返回：有
    } else if (m_shared_credit_limit == 0 || m_shared_credit < m_shared_credit_limit) { // 共享信用限制 == 0 || 该分区（DRAM channel）的共享信用 < 共享信用限制
        return true;    // 有
    } else {
        return false; 
    }
}

void memory_partition_unit::arbitration_metadata::borrow_credit(int inner_sub_partition_id)     // 子分区（L2 cache）借入信用（信用？我觉得用空间会更好一些）
{   // 信用增加并不是好事，因为存在信用限制，在判断的时候， 信用 < 信用限制
    int spid = inner_sub_partition_id; 
    if (m_private_credit[spid] < m_private_credit_limit) {  // 判断： 私人信用 < 私人信用限制
        m_private_credit[spid] += 1;    // 个人信用+1
    } else if (m_shared_credit_limit == 0 || m_shared_credit < m_shared_credit_limit) { // 共享信用 < 共享信用限制
        m_shared_credit += 1;           // 共享信用+1
    } else {
        assert(0 && "DRAM arbitration error: Borrowing from depleted credit!"); 
    }
    m_last_borrower = spid; // 记录一下，最后一个借入信用的L2 cache的本地ID 
}

void memory_partition_unit::arbitration_metadata::return_credit(int inner_sub_partition_id) 
{
    int spid = inner_sub_partition_id; 
    if (m_private_credit[spid] > 0) {
        m_private_credit[spid] -= 1; 
    } else {
        m_shared_credit -= 1; 
    } 
    assert((m_shared_credit >= 0) && "DRAM arbitration error: Returning more than available credits!"); 
}

void memory_partition_unit::arbitration_metadata::print( FILE *fp ) const 
{
    fprintf(fp, "private_credit = "); 
    for (unsigned p = 0; p < m_private_credit.size(); p++) {
        fprintf(fp, "%d ", m_private_credit[p]); 
    }
    fprintf(fp, "(limit = %d)\n", m_private_credit_limit); 
    fprintf(fp, "shared_credit = %d (limit = %d)\n", m_shared_credit, m_shared_credit_limit); 
}

bool memory_partition_unit::busy() const 
{
    bool busy = false; 
    for (unsigned p = 0; p < m_config->m_n_sub_partition_per_memory_channel; p++) {
        if (m_sub_partition[p]->busy()) {
            busy = true; 
        }
    }
    return busy; 
}

void memory_partition_unit::cache_cycle(unsigned cycle) 
{
    for (unsigned p = 0; p < m_config->m_n_sub_partition_per_memory_channel; p++) {
        m_sub_partition[p]->cache_cycle(cycle); 
    }
}

void memory_partition_unit::visualizer_print( gzFile visualizer_file ) const 
{
    m_dram->visualizer_print(visualizer_file);
    for (unsigned p = 0; p < m_config->m_n_sub_partition_per_memory_channel; p++) {
        m_sub_partition[p]->visualizer_print(visualizer_file); 
    }
}

// determine whether a given subpartition can issue to DRAM 
bool memory_partition_unit::can_issue_to_dram(int inner_sub_partition_id)   // 确定能否发射给传入的子分区（L2 cache）
{   // DRAM-to-L2队列满了为什么会产生争用？因为之前判断L2-to-DRAM队列不空，也就是说，将有数据/指令从L2-to-DRAM队列进入到DRAM，在DRAM执行完成后，返回到L2 cache，这个过程需要DRAM-to-L2队列的参与，若DRAM-to-L2队列满了，则新产生的数据/指令，送不出去
    int spid = inner_sub_partition_id;  // L2 cache的本地ID
    bool sub_partition_contention = m_sub_partition[spid]->dram_L2_queue_full();    // 判断子分区是否存在争用（查看DRAM-to-L2队列是否满了）
    bool has_dram_resource = m_arbitration_metadata.has_credits(spid);              // 判断该子分区（L2 cache）是否有信用
    // 之前判断L2-to-DRAM队列不空，即：将有数据/指令从L2-to-DRAM队列进入到DRAM。在DRAM执行完成后，返回到L2 cache，这个过程需要DRAM-to-L2队列的参与，若DRAM-to-L2队列满了，则新产生的数据/指令，送不出去
    MEMPART_DPRINTF("sub partition %d sub_partition_contention=%c has_dram_resource=%c\n", 
                    spid, (sub_partition_contention)? 'T':'F', (has_dram_resource)? 'T':'F'); 
    // 不存在争用 && （信用额度 < 信用限制）
    return (has_dram_resource && !sub_partition_contention); 
}

int memory_partition_unit::global_sub_partition_id_to_local_id(int global_sub_partition_id) const   // L2 cache（主存子分区）的全局ID转换为本地ID
{   // 全局子分区（L2 cache）ID - 当前DRAM channel（主存分区）的ID * 每个DRAM channel所含子分区（L2 cache）的数量
    return (global_sub_partition_id - m_id * m_config->m_n_sub_partition_per_memory_channel);  
}

void memory_partition_unit::dram_cycle()    // 根据命名，可推知：L2cache == 主存子分区， L2 cache应该是DRAM上面的片上cache
{   // 整个函数都是在DRAM的时钟域内运行的，因此，涉及到的所有部件应该都是在DRAM channel上：（1）DRAM返回队列    （2）DRAM-to-L2队列
    // 将已经完成的内存访问（取指令/取数据/写数据的确认）弹出，并将其push到DRAM-L2队列                            // pop completed memory request from dram and push it to dram-to-L2 queue
    // DRAM-to-L2队列存在于DRAM channel上，因为运行在DRAM的时钟域！！！！！！！！！！！！                       // of the original sub partition
    mem_fetch* mf_return = m_dram->return_queue_top();  // 从DRAM返回队列中取出队首元素mf
    if (mf_return) {    // 判断取出来的mf是否为空
        unsigned dest_global_spid = mf_return->get_sub_partition_id();  // 获取mf传输的目的L2 cache的全局ID， 之前有过类似的例子，mf需要传输到指定硬件（CORE）上面执行，于是有了shader的概念，实际上：shader == CORE，只不过shader采用全局ID，CORE则是采用局部的
        int dest_spid = global_sub_partition_id_to_local_id(dest_global_spid);  // L2 cache全局ID转为本地ID
        assert(m_sub_partition[dest_spid]->get_id() == dest_global_spid);       // 通过本地ID找到L2 cache，并返回该L2CACHE的全局ID，检查
        if (!m_sub_partition[dest_spid]->dram_L2_queue_full()) {// 查看DRAM-TO-L2队列是否满了
            if( mf_return->get_access_type() == L1_WRBK_ACC ) { // 查看mf的访问类型：L1 cache写回（即：从CORE的L1 CACHE中写回数据到DRAM）（数据型，另一种已知的是：INST_ACC_R，表示mf是一条指令）
                m_sub_partition[dest_spid]->set_done(mf_return);// mf设置为：执行完成
                delete mf_return;   // 删除mf
            } else {                // 其他类型的指令处理：
                m_sub_partition[dest_spid]->dram_L2_queue_push(mf_return);                              // 将mf放入DRAM-to-L2队列
                mf_return->set_status(IN_PARTITION_DRAM_TO_L2_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);   // 设置mf的状态（移动到了另一个部件（L2 cache））
                m_arbitration_metadata.return_credit(dest_spid);            // L2 cache归还信用
                MEMPART_DPRINTF("mem_fetch request %p return from dram to sub partition %d\n", mf_return, dest_spid); 
            }
            m_dram->return_queue_pop();     // 弹出mf
        }
    } else {    // mf为空
        m_dram->return_queue_pop();     // mf是空的，直接弹出
    }   // 小结：这部分的操作是将”DRAM返回队列“的mf传输到”指定ID的L2 cache”的“DRAM-to-L2队列”

    m_dram->cycle();                // 运行DRAM channel
    m_dram->dram_log(SAMPLELOG);    // 日志文件 

    if( !m_dram->full() ) {         // 判断DRAM channel是否满了
        // L2-to-DRAM队列             /// L2->DRAM queue to DRAM latency queue 
        // 在多个L2 cache之间的仲裁     /// Arbitrate among multiple L2 subpartitions 
        int last_issued_partition = m_arbitration_metadata.last_borrower(); // 最后发射的L2 cache的ID（最后一个来借信用的L2 cache的本地ID）
        for (unsigned p = 0; p < m_config->m_n_sub_partition_per_memory_channel; p++) { // 遍历DRAM channel内的所有子分区（L2 cache）
            int spid = (p + last_issued_partition + 1) % m_config->m_n_sub_partition_per_memory_channel;    // 选取：最后发射的L2 cache之后的下一个L2 cache 
            if (!m_sub_partition[spid]->L2_dram_queue_empty() && can_issue_to_dram(spid)) { // L2-to-DRAM队列不空 && 可以发射到ID为spid的L2 cache
                mem_fetch *mf = m_sub_partition[spid]->L2_dram_queue_top(); // 从L2-to-DRAM队列中取出队首元素
                m_sub_partition[spid]->L2_dram_queue_pop();                 // 弹出该元素
                MEMPART_DPRINTF("Issue mem_fetch request %p from sub partition %d to dram\n", mf, spid); 
                dram_delay_t d;                                                                     // 创建“DRAM延时队列”元素
                d.req = mf;                                                                         // 请求指针指向mf
                d.ready_cycle = gpu_sim_cycle+gpu_tot_sim_cycle + m_config->dram_latency;           // 记录时间
                m_dram_latency_queue.push_back(d);                                                  // 进入“DRAM延时队列”
                mf->set_status(IN_PARTITION_DRAM_LATENCY_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);    // 修改mf的状态
                m_arbitration_metadata.borrow_credit(spid);                                         // 修改该L2 cache（spid）的信用
                break;  // DRAM channel在一个cycle内，只能处理一个请求     /// the DRAM should only accept one request per cycle
            }
        }
    }

    // DRAM latency queue   ”DRAM延时队列“不空 && 当前时间 >= ”DRAM延时队列“队首元素的准备时间（也就是时延） &&  DRAM channel不满
    if( !m_dram_latency_queue.empty() && ( (gpu_sim_cycle+gpu_tot_sim_cycle) >= m_dram_latency_queue.front().ready_cycle ) && !m_dram->full() ) {
        mem_fetch* mf = m_dram_latency_queue.front().req;   // 取出”DRAM延时队列“队首元素mf
        m_dram_latency_queue.pop_front();                   // 弹出该元素mf
        m_dram->push(mf);                                   // mf放入DRAM channel
    }
}

void memory_partition_unit::set_done( mem_fetch *mf )   // mf执行完毕
{
    unsigned global_spid = mf->get_sub_partition_id();  // 获取子分区（L2 cache）的全局ID
    int spid = global_sub_partition_id_to_local_id(global_spid);    // 全局ID转换为本地ID
    assert(m_sub_partition[spid]->get_id() == global_spid); 
    if (mf->get_access_type() == L1_WRBK_ACC || mf->get_access_type() == L2_WRBK_ACC) { // L1/L2 cache的写回操作（将L1/L2 cache的数据写回到DRAM中）
        m_arbitration_metadata.return_credit(spid);     // L2 cache 归还信用
        MEMPART_DPRINTF("mem_fetch request %p return from dram to sub partition %d\n", mf, spid); 
    }
    m_sub_partition[spid]->set_done(mf);    // 执行完毕
}

void memory_partition_unit::set_dram_power_stats(unsigned &n_cmd,       // DRAM channel执行cycle的次数
                                                 unsigned &n_activity,  // DRAM channel中非空的mrq的数量（bank 与 mrq是一一对应的）
                                                 unsigned &n_nop,       // DRAM channel的Nop次数（访存请求mrq是空的，不需要处理，空转）
                                                 unsigned &n_act,       // DRAM channel的行动
                                                 unsigned &n_pre,       // DRAM channel的预处理
                                                 unsigned &n_rd,        // DRAM channel的“读取”操作
                                                 unsigned &n_wr,        // DRAM channel的”写入“操作
                                                 unsigned &n_req) const // DRAM channel接收的”访存请求“
{
    m_dram->set_dram_power_stats(n_cmd, n_activity, n_nop, n_act, n_pre, n_rd, n_wr, n_req);
}

void memory_partition_unit::print( FILE *fp ) const
{
    fprintf(fp, "Memory Partition %u: \n", m_id); 
    for (unsigned p = 0; p < m_config->m_n_sub_partition_per_memory_channel; p++) {
        m_sub_partition[p]->print(fp); 
    }
    fprintf(fp, "In Dram Latency Queue (total = %zd): \n", m_dram_latency_queue.size()); 
    for (std::list<dram_delay_t>::const_iterator mf_dlq = m_dram_latency_queue.begin(); 
         mf_dlq != m_dram_latency_queue.end(); ++mf_dlq) {
        mem_fetch *mf = mf_dlq->req; 
        fprintf(fp, "Ready @ %llu - ", mf_dlq->ready_cycle); 
        if (mf) 
            mf->print(fp); 
        else 
            fprintf(fp, " <NULL mem_fetch?>\n"); 
    }
    m_dram->print(fp); 
}

memory_sub_partition::memory_sub_partition( unsigned sub_partition_id, 
                                            const struct memory_config *config,
                                            class memory_stats_t *stats )
{
    m_id = sub_partition_id;
    m_config=config;
    m_stats=stats;

    assert(m_id < m_config->m_n_mem_sub_partition); 

    char L2c_name[32];
    snprintf(L2c_name, 32, "L2_bank_%03d", m_id);
    m_L2interface = new L2interface(this);
    m_mf_allocator = new partition_mf_allocator(config);

    if(!m_config->m_L2_config.disabled())
       m_L2cache = new l2_cache(L2c_name,m_config->m_L2_config,-1,-1,m_L2interface,m_mf_allocator,IN_PARTITION_L2_MISS_QUEUE);

    unsigned int icnt_L2;
    unsigned int L2_dram;
    unsigned int dram_L2;
    unsigned int L2_icnt;
    sscanf(m_config->gpgpu_L2_queue_config,"%u:%u:%u:%u", &icnt_L2,&L2_dram,&dram_L2,&L2_icnt );
    m_icnt_L2_queue = new fifo_pipeline<mem_fetch>("icnt-to-L2",0,icnt_L2); 
    m_L2_dram_queue = new fifo_pipeline<mem_fetch>("L2-to-dram",0,L2_dram);
    m_dram_L2_queue = new fifo_pipeline<mem_fetch>("dram-to-L2",0,dram_L2);
    m_L2_icnt_queue = new fifo_pipeline<mem_fetch>("L2-to-icnt",0,L2_icnt);
    wb_addr=-1;
}

memory_sub_partition::~memory_sub_partition()
{
    delete m_icnt_L2_queue;
    delete m_L2_dram_queue;
    delete m_dram_L2_queue;
    delete m_L2_icnt_queue;
    delete m_L2cache;
    delete m_L2interface;
}

void memory_sub_partition::cache_cycle( unsigned cycle )    // 运行主存子分区（L2 cache）
{
    // ************************** L2 cache响应mshr（之前未命中的访问请求mf（已经访问完成了））  将其放入：“L2-to-ICNT队列”               /// L2 fill responses
    if( !m_config->m_L2_config.disabled()) {    // 判断L2 cache的使能有效位
        if ( m_L2cache->access_ready() && !m_L2_icnt_queue->full() ) {   // L2 cache访问准备好了  &&  “L2-to-ICNT队列”未满 
            mem_fetch *mf = m_L2cache->next_access();    // 从mshr中弹出访问请求（这些请求都已经访存完毕，准备进入ICNT）（mshr：Miss Status Holding Registers ，一种特殊寄存器，用于保存：未命中L2 cache的访存请求（已访存）。当访存请求在L2 cache中未命中时，就会被添加到mshr）
            if(mf->get_access_type() != L2_WR_ALLOC_R){  // 不要将“写入分配读取请求”传递回上级缓存（L1 cache） /// Don't pass write allocate read request back to upper level cache
                mf->set_reply();            // 回应mf
                mf->set_status(IN_PARTITION_L2_TO_ICNT_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);  // 设置mf的状态
                m_L2_icnt_queue->push(mf);  // mf进入“L2-to-ICNT队列”（FIFO pipeline）
            } else{  // L2_WR_ALLOC_R类型的mf直接删除
                m_request_tracker.erase(mf);
                delete mf;
            }
        }
    }

    // ************************** 查看 ”DRAM-to-L2队列“    将”DRAM-to-L2队列“的元素送入”L2 cache“ 或者 ”L2-to-ICNT队列“（不存在L2 cache） /// DRAM to L2 (texture) and icnt (not texture)
    if ( !m_dram_L2_queue->empty() ) {  // 不空，执行if
        mem_fetch *mf = m_dram_L2_queue->top(); // 取出”DRAM-to-L2队列“队首元素
        if ( !m_config->m_L2_config.disabled() && m_L2cache->waiting_for_fill(mf) ) {   // L2 cache的使能有效 && mf等待进入更低一级的内存（L2 cache？）
            if (m_L2cache->fill_port_free()) {  // 填充端口是否空闲， 空闲执行if
                mf->set_status(IN_PARTITION_L2_FILL_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle); // 设置mf状态
                m_L2cache->fill(mf,gpu_sim_cycle+gpu_tot_sim_cycle);    // 将mf放入L2 cache
                m_dram_L2_queue->pop(); // ”DRAM-to-L2队列“弹出mf
            }
        } else if ( !m_L2_icnt_queue->full() ) {    // 端口不空闲，则查看：”L2-to-ICNT队列“是否满，（直接越过L2 cache，到这个队列）
            mf->set_status(IN_PARTITION_L2_TO_ICNT_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);  // 设置mf的状态
            m_L2_icnt_queue->push(mf);  // mf放入”L2-to-ICNT队列“
            m_dram_L2_queue->pop();     // ”DRAM-to-L2队列“弹出mf
        }
    }

    // ************************** 运行L2 cache ，处理未命中的L2 cache的访存请求,放入低一级内存 **************************   /// prior L2 misses inserted into m_L2_dram_queue here
    if( !m_config->m_L2_config.disabled() )
       m_L2cache->cycle();  // 处理”未命中队列“的元素， 放入低一级内存

    // ************************** 处理新的“L2纹理访问” or “非纹理访问”  从L2 cache读取数据到mf的数据域 / 将mf的数据域内容写入L2 cache **************************    /// new L2 texture accesses and/or non-texture accesses
    if ( !m_L2_dram_queue->full() && !m_icnt_L2_queue->empty() ) {  // “L2-to-DRAM队列”不满 && “ICNT-to-L2队列”不空
        mem_fetch *mf = m_icnt_L2_queue->top();     // 取出“ICNT-to-L2队列”的元素
        if ( !m_config->m_L2_config.disabled() &&   // L2 cache使能有效
              ( (m_config->m_L2_texure_only && mf->istexture()) || (!m_config->m_L2_texure_only) )  // （L2 cache仅仅保存纹理数据 && mf是纹理类型） || （L2 cache不仅仅保存纹理数据）
           ) {
            // 准备访问L2 cache     /// L2 is enabled and access is for L2
            bool output_full = m_L2_icnt_queue->full();     // 查询“L2-to-ICNT队列”是否满了
            bool port_free = m_L2cache->data_port_free();   // 查询L2 cache的数据端口是否可用
            if ( !output_full && port_free ) {  // “L2-to-ICNT队列”未满 && L2 cache的数据端口可用
                std::list<cache_event> events;  // 事件list，记录在L2 cache的访问过程
                enum cache_request_status status = m_L2cache->access(mf->get_addr(),mf,gpu_sim_cycle+gpu_tot_sim_cycle,events); // L2 cache访问，读取数据存放到mf的数据域 / 将mf数据域的内容写入L2 cache
                bool write_sent = was_write_sent(events);   // 判断是否为写请求
                bool read_sent = was_read_sent(events);     // 判断是否为读请求

                if ( status == HIT ) {  // L2 cache命中
                    if( !write_sent ) { // 不是写入请求
                        // L2 cache回复      /// L2 cache replies
                        assert(!read_sent); // 不是读取请求？？？
                        if( mf->get_access_type() == L1_WRBK_ACC ) {    // L1 cache写回？（为什么没有写回到主存就结束？）
                            m_request_tracker.erase(mf);    // 删除mf的跟踪
                            delete mf;                      // 删除mf
                        } else {
                            mf->set_reply();    // 变更mf的类型（L2 cache访问命中且完成数据的写入/读取）
                            mf->set_status(IN_PARTITION_L2_TO_ICNT_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);  // 设置mf的状态
                            m_L2_icnt_queue->push(mf);  // mf进入“L2-to-ICNT队列”
                        }
                        m_icnt_L2_queue->pop();         // “ICNT-to-L2队列”弹出该mf
                    } else {    // 写入请求
                        assert(write_sent);
                        m_icnt_L2_queue->pop();         // “ICNT-to-L2队列”弹出该mf，（写入需要一个ACK回复，为什么直接弹出了？？）
                    }
                } else if ( status != RESERVATION_FAIL ) {
                    // L2 cache accepted request
                    m_icnt_L2_queue->pop();
                } else {
                    assert(!write_sent);
                    assert(!read_sent);
                    // L2 cache lock-up: will try again next cycle 下次尝试
                }
            }
        } else {    // L2 cache使能无效 || ”非纹理请求“访问“纹理L2 cache”
            // L2 is disabled or non-texture access to texture-only L2
            mf->set_status(IN_PARTITION_L2_TO_DRAM_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);  // 设置mf状态
            m_L2_dram_queue->push(mf);  // 跳过L2 cache，直接进入“L2-to-DRAM队列”
            m_icnt_L2_queue->pop();     // 弹出mf
        }
    }

    // ************************** ROP延时队列 /// ROP delay queue 
    if( !m_rop.empty() && (cycle >= m_rop.front().ready_cycle) && !m_icnt_L2_queue->full() ) {  // ROP队列不空 && 队首元素的延时时间到了 && “ICNT-to-L2”队列不满
        mem_fetch* mf = m_rop.front().req;  // 取出”ROP延时队列“的请求mf
        m_rop.pop();                        // 弹出mf
        m_icnt_L2_queue->push(mf);          // mf进入“ICNT-to-L2队列”
        mf->set_status(IN_PARTITION_ICNT_TO_L2_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);  // 设置mf的状态
    }
}

bool memory_sub_partition::full() const
{
    return m_icnt_L2_queue->full();
}

bool memory_sub_partition::L2_dram_queue_empty() const
{
   return m_L2_dram_queue->empty(); 
}

class mem_fetch* memory_sub_partition::L2_dram_queue_top() const
{
   return m_L2_dram_queue->top(); 
}

void memory_sub_partition::L2_dram_queue_pop() 
{
   m_L2_dram_queue->pop(); 
}

bool memory_sub_partition::dram_L2_queue_full() const
{
   return m_dram_L2_queue->full(); 
}

void memory_sub_partition::dram_L2_queue_push( class mem_fetch* mf )
{
   m_dram_L2_queue->push(mf); 
}

void memory_sub_partition::print_cache_stat(unsigned &accesses, unsigned &misses) const
{
    FILE *fp = stdout;
    if( !m_config->m_L2_config.disabled() )
       m_L2cache->print(fp,accesses,misses);
}

void memory_sub_partition::print( FILE *fp ) const
{
    if ( !m_request_tracker.empty() ) {
        fprintf(fp,"Memory Sub Parition %u: pending memory requests:\n", m_id);
        for ( std::set<mem_fetch*>::const_iterator r=m_request_tracker.begin(); r != m_request_tracker.end(); ++r ) {
            mem_fetch *mf = *r;
            if ( mf )
                mf->print(fp);
            else
                fprintf(fp," <NULL mem_fetch?>\n");
        }
    }
    if( !m_config->m_L2_config.disabled() )
       m_L2cache->display_state(fp);
}

void memory_stats_t::visualizer_print( gzFile visualizer_file )
{
   // gzprintf(visualizer_file, "Ltwowritemiss: %d\n", L2_write_miss);
   // gzprintf(visualizer_file, "Ltwowritehit: %d\n",  L2_write_access-L2_write_miss);
   // gzprintf(visualizer_file, "Ltworeadmiss: %d\n", L2_read_miss);
   // gzprintf(visualizer_file, "Ltworeadhit: %d\n", L2_read_access-L2_read_miss);
   if (num_mfs)
      gzprintf(visualizer_file, "averagemflatency: %lld\n", mf_total_lat/num_mfs);
}

void gpgpu_sim::print_dram_stats(FILE *fout) const
{
	unsigned cmd=0;
	unsigned activity=0;
	unsigned nop=0;
	unsigned act=0;
	unsigned pre=0;
	unsigned rd=0;
	unsigned wr=0;
	unsigned req=0;
	unsigned tot_cmd=0;
	unsigned tot_nop=0;
	unsigned tot_act=0;
	unsigned tot_pre=0;
	unsigned tot_rd=0;
	unsigned tot_wr=0;
	unsigned tot_req=0;

	for (unsigned i=0;i<m_memory_config->m_n_mem;i++){
		m_memory_partition_unit[i]->set_dram_power_stats(cmd,activity,nop,act,pre,rd,wr,req);
		tot_cmd+=cmd;
		tot_nop+=nop;
		tot_act+=act;
		tot_pre+=pre;
		tot_rd+=rd;
		tot_wr+=wr;
		tot_req+=req;
	}
    fprintf(fout,"gpgpu_n_dram_reads = %d\n",tot_rd );
    fprintf(fout,"gpgpu_n_dram_writes = %d\n",tot_wr );
    fprintf(fout,"gpgpu_n_dram_activate = %d\n",tot_act );
    fprintf(fout,"gpgpu_n_dram_commands = %d\n",tot_cmd);
    fprintf(fout,"gpgpu_n_dram_noops = %d\n",tot_nop );
    fprintf(fout,"gpgpu_n_dram_precharges = %d\n",tot_pre );
    fprintf(fout,"gpgpu_n_dram_requests = %d\n",tot_req );
}

unsigned memory_sub_partition::flushL2() 
{ 
    if (!m_config->m_L2_config.disabled()) {
        m_L2cache->flush(); 
    }
    return 0; // L2 is read only in this version
}

bool memory_sub_partition::busy() const 
{
    return !m_request_tracker.empty();
}

void memory_sub_partition::push( mem_fetch* req, unsigned long long cycle )     // cycle：当前cycle轮次， req：访存mf    该函数负责将访存mf放入主存子分区。     根据req(mf)的类型，决定放入“rop队列”（存在时延）还是“ICNT-to-L2队列”
{
    if (req) {  // 访存请求req不能为空
        m_request_tracker.insert(req);          // 跟踪req
        m_stats->memlatstat_icnt2mem_pop(req);  //
        if( req->istexture() ) {    // 判断是否为纹理请求， 是，则执行if分支
            m_icnt_L2_queue->push(req);     // req直接进入“ICNT-to-L2队列”
            req->set_status(IN_PARTITION_ICNT_TO_L2_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle); // 设置req的状态
        } else {    // 否，执行else分支
            rop_delay_t r;  // 创建 “rop时延请求”r
            r.req = req;    // 设置r的请求域
            r.ready_cycle = cycle + m_config->rop_latency;  // 设置r的时延
            m_rop.push(r);  // 放入rop队列
            req->set_status(IN_PARTITION_ROP_DELAY,gpu_sim_cycle+gpu_tot_sim_cycle);        // 设置req的状态
        }
    }
}

mem_fetch* memory_sub_partition::pop() 
{
    mem_fetch* mf = m_L2_icnt_queue->pop();
    m_request_tracker.erase(mf);
    if ( mf && mf->isatomic() )
        mf->do_atomic();
    if( mf && (mf->get_access_type() == L2_WRBK_ACC || mf->get_access_type() == L1_WRBK_ACC) ) {
        delete mf;
        mf = NULL;
    } 
    return mf;
}

mem_fetch* memory_sub_partition::top() 
{
    mem_fetch *mf = m_L2_icnt_queue->top();
    if( mf && (mf->get_access_type() == L2_WRBK_ACC || mf->get_access_type() == L1_WRBK_ACC) ) {
        m_L2_icnt_queue->pop();
        m_request_tracker.erase(mf);
        delete mf;
        mf = NULL;
    } 
    return mf;
}

void memory_sub_partition::set_done( mem_fetch *mf )    // 清除L2 CACHE的“请求跟踪集合”中的mf
{
    m_request_tracker.erase(mf);
}

void memory_sub_partition::accumulate_L2cache_stats(class cache_stats &l2_stats) const {
    if (!m_config->m_L2_config.disabled()) {
        l2_stats += m_L2cache->get_stats();
    }
}

void memory_sub_partition::get_L2cache_sub_stats(struct cache_sub_stats &css) const{
    if (!m_config->m_L2_config.disabled()) {
        m_L2cache->get_sub_stats(css);
    }
}

void memory_sub_partition::visualizer_print( gzFile visualizer_file )
{
    // TODO: Add visualizer stats for L2 cache 
}

