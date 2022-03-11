/* adlist.h - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */

// 定义 redis 中的双向链表结构体
typedef struct listNode {
    // 前驱节点
    struct listNode *prev;
    // 后继节点
    struct listNode *next;
    // 节点值
    void *value;
} listNode;

// 定义 list 节点迭代器
typedef struct listIter {
    listNode *next; // 指向下一个节点
    int direction;  // 迭代器，正向反向
} listIter;

// 定义 list 结构体，辅助操作 list
typedef struct list {
    listNode *head;                      // 表头指针
    listNode *tail;                      // 表尾指针
    void *(*dup)(void *ptr);             // 节点值复制函数
    void (*free)(void *ptr);             // 节点值释放函数（函数指针）
    int (*match)(void *ptr, void *key);  // 节点值对比函数
    unsigned long len;                   // 链表包含的节点数量
} list;

/* Functions implemented as macros */
#define listLength(l) ((l)->len)                    // 获取 list 中包含的 node 数量
#define listFirst(l) ((l)->head)                    // 获取 list 头节点指针
#define listLast(l) ((l)->tail)                     // 获取 list 尾节点指针
#define listPrevNode(n) ((n)->prev)                 // 获取当前节点的前驱节点
#define listNextNode(n) ((n)->next)                 // 获得当前节点的后继节点
#define listNodeValue(n) ((n)->value)

#define listSetDupMethod(l,m) ((l)->dup = (m))      // 指定节点复制函数
#define listSetFreeMethod(l,m) ((l)->free = (m))    // 指定节点释放函数
#define listSetMatchMethod(l,m) ((l)->match = (m))  // 指定节点的比较函数

#define listGetDupMethod(l) ((l)->dup)   // 获得节点复制函数
#define listGetFree(l) ((l)->free)
#define listGetMatchMethod(l) ((l)->match)

/* Prototypes 函数原型*/
list *listCreate(void); // 创建一个不包含任何节点的新链表
void listRelease(list *list); // 释放给定链表，以及链表中的所有节点

// CRUD 操作
list *listAddNodeHead(list *list, void *value);
list *listAddNodeTail(list *list, void *value);
list *listInsertNode(list *list, listNode *old_node, void *value, int after);
void listDelNode(list *list, listNode *node); // O(N)

listIter *listGetIterator(list *list, int direction);
listNode *listNext(listIter *iter);
void listReleaseIterator(listIter *iter);
list *listDup(list *orig);
listNode *listSearchKey(list *list, void *key); // O(N)
listNode *listIndex(list *list, long index);  // O(N)
void listRewind(list *list, listIter *li);
void listRewindTail(list *list, listIter *li);
void listRotate(list *list);

/* Directions for iterators */
#define AL_START_HEAD 0  // 遍历从头开始
#define AL_START_TAIL 1  // 遍历从尾开始

#endif /* __ADLIST_H__ */
