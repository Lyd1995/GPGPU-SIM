// Copyright (c) 2009-2011, Wilson W.L. Fung, Tor M. Aamodt
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

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

#ifndef DELAYQUEUE_H
#define DELAYQUEUE_H

#include "../statwrapper.h"
#include "gpu-misc.h"

template <class T>
struct fifo_data {
   T *m_data;
   fifo_data *m_next;
};

template <class T> 
class fifo_pipeline {
public:
   fifo_pipeline(const char* nm, unsigned int minlen, unsigned int maxlen ) 
   {
      assert(maxlen);
      m_name = nm;
      m_min_len = minlen;  // 流水线最小长度
      m_max_len = maxlen;  // 流水线最大长度
      m_length = 0;     // 流水线长度
      m_n_element = 0;  // 初始化为0，
      m_head = NULL;
      m_tail = NULL;
      for (unsigned i=0;i<m_min_len;i++) 
         push(NULL);    // 压入： NULL
   }

   ~fifo_pipeline() 
   {
      while (m_head) {
         m_tail = m_head;
         m_head = m_head->m_next;
         delete m_tail;
      }
   }

   void push(T* data )  // FIFO pipeline的push操作
   {
      assert(m_length < m_max_len); // 长度必须小于最大长度
      if (m_head) {     // 以链表的形式组织， 判断表头是否为：NULL。表头不空执行if
         if (m_tail->m_data || m_length < m_min_len) {   // 末尾元素的数据域不空 ||  当前长度 < 最小长度
            m_tail->m_next = new fifo_data<T>();   // 末尾元素的数据域不是空的，不能直接插入，应该创建一个新的节点，让m_tail指向它
            m_tail = m_tail->m_next;
            m_length++;       // NULL节点也计入长度
            m_n_element++;    // NULL节点也计入，但是每次弹出NULL节点时，相对地会-1。m_n_element：最后应该是表示非空的元素个数
         }
      } else {          // 表头是空的，初始化链表
         m_head = m_tail = new fifo_data<T>();  // 
         m_length++;          // NULL节点也计入长度
         m_n_element++;       // NULL节点也计入，但是每次弹出NULL节点时，相对地会-1。  m_n_element ：最后应该是表示非空的元素个数。  查看过代码，如果push一个NULL进FIFO pipeline，m_n_element会先--，和这边的++抵消掉
      }
      m_tail->m_next = NULL;  // 尾插法，每次将新的元素插入到末尾m_tail
      m_tail->m_data = data;  // 数据域
   }

   T* pop()    // FIFO pipeline的pop操作
   {
      fifo_data<T>* next;  // 因为需要弹出第一个元素（即：表头），所以设置一个指针来接替
      T* data;             // 返回的数据
      if (m_head) {  // 判断表头是否为空
         next = m_head->m_next;   // 新的表头
         data = m_head->m_data;   // 获取数据
         if ( m_head == m_tail ) {   // 表头 == 表尾， 链表中只有一个元素，这个时候需要设置一下表尾
            assert( next == NULL );
            m_tail = NULL;           // 表尾设置为空  
         }
         delete m_head;           // 删除表头
         m_head = next;           // 设置表头
         m_length--;              // 修改链表长度
         if (m_length == 0) {     // 长度 == 0
            assert( m_head == NULL );
            m_tail = m_head;
         }
         m_n_element--;           // 有效元素个数-1
         if (m_min_len && m_length < m_min_len) {
            push(NULL);    // 如果 链表长度 小于 设定的最小长度   则push空节点（数据域为空）
            m_n_element--; // 不计入插入的NULL （因为在push里面，无论push什么，m_n_element都会+1）  /// uncount NULL elements inserted to create delays
         }
      } else { // 表头为空， 链表（FIFO pipeline）中不存在数据
         data = NULL;   // 数据为空
      }
      return data;   // 返回
   }

   T* top() const
   {
      if (m_head) {
         return m_head->m_data;
      } else {
         return NULL;
      }
   }

   void set_min_length(unsigned int new_min_len)   // 设置FIFO pipeline的最小长度
   {
      if (new_min_len == m_min_len) return;  // 对比： 之前的最小长度  ==  新的最小长度 
   
      if (new_min_len > m_min_len) {         // 对比： 新的最小长度   >   之前的最小长度
         m_min_len = new_min_len;            // 更新最小长度   最小长度增加
         while (m_length < m_min_len) {      // m_length：当前FIFO pipeline的长度（链表长度），如果小于设置的“最小长度”进入循环。   通过压入NULL，增加“链表长度”，使“链表长度” == “设定的最小长度”
            push(NULL);                      // 压入： NULL（相当于流水线停顿/空转）
            m_n_element--;    // m_n_element：统计FIFO pipeline中的非NULL节点个数   /// uncount NULL elements inserted to create delays
         }
      } else {    // 对比： 新的最小长度   <   之前的最小长度
         // in this branch imply that the original min_len is larger then 0
         // ie. head != 0
         assert(m_head);            // 判断表头是否为空
         m_min_len = new_min_len;   // 更新最小长度   最小长度减小
         while ((m_length > m_min_len) && (m_tail->m_data == 0)) {   // 删减链表，去掉链表尾部，数据域为空的节点。   通过不断删除：（1）空节点  （2）数据域为空的节点。 减少“链表长度”， 使“链表长度“ == “设定的最小长度”
            fifo_data<T> *iter;  // 迭代器
            iter = m_head;       // 表头
            while (iter && (iter->m_next != m_tail))  // 找出空节点
               iter = iter->m_next;
            if (!iter) {   // iter == NULL 执行下面的操作
               // 此时，链表中只有一个节点，且这个节点的数据域是空的  /// there is only one node, and that node is empty
               assert(m_head->m_data == 0);
               pop();   // 弹出数据域为空的节点
            } else {       // iter不空， 且至少两个节点
               // 此时，链表中有多个节点，尾节点是空的节点 // there are more than one node, and tail node is empty
               assert(iter->m_next == m_tail);
               delete m_tail;       // 释放尾节点的空间
               m_tail = iter;       // 尾节点指针指向尾节点的前一节点
               m_tail->m_next = 0;  // 末节点设为空
               m_length--;          // 链表长度--
            }
         }
      }
   }

   bool full() const { return (m_max_len && m_length >= m_max_len); }   // 判断FIFO pipeline是否满了。   链表长度 >= 最大长度
   bool empty() const { return m_head == NULL; }                        // 判断FIFO pipeline是否空了。   查看表头是否为空
   unsigned get_n_element() const { return m_n_element; }               // 获取有效节点的个数
   unsigned get_length() const { return m_length; }                     // 获取链表的长度（包括空节点（数据域为空））
   unsigned get_max_len() const { return m_max_len; }                   // 获取链表的最大长度

   void print() const
   {
      fifo_data<T>* ddp = m_head;
      printf("%s(%d): ", m_name, m_length);
      while (ddp) {
         printf("%p ", ddp->m_data);
         ddp = ddp->m_next;
      }
      printf("\n");
   }

private:
   const char* m_name;        // 名字？

   unsigned int m_min_len;    // 链表的最小长度
   unsigned int m_max_len;    // 链表的最大长度
   unsigned int m_length;     // 链表的长度（包括空节点（数据域为空））
   unsigned int m_n_element;  // 有效节点的个数

   fifo_data<T> *m_head;      // 链表头部（表头），队首
   fifo_data<T> *m_tail;      // 链表尾部（表尾），队尾
};

#endif
