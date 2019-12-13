//公共模块定义处
#pragma once
#include<iostream>
#include<assert.h>
#include<Windows.h>
using namespace std;
//65537
const size_t MAX_SIZE = 64 * 1024; //64k以下的从threadcache申请0-16页
const size_t NFREE_LIST = MAX_SIZE / 8; //8k，表明链表的最大长度
const size_t MAX_PAGES = 129;//申请的最大页数
const size_t PAGE_SHIFT = 12; //4k为页的位移

inline void*& NextObj(void* obj)
{
	//返回obj指向空间前四/八个字节的指针,也就是下一个节点的指针
	return *(void**)obj;
}
//这个类是一个存放固定大小对象内存空间的链表
class FreeList
{
public:
	void Push(void* obj)
	{
		//头插
		NextObj(obj) = _freelist;
		_freelist = obj;
	}

	void PushRange(void* head, void* tail)
	{
		//头插
		//_freelist指向的地址赋值给tail指向结点的前四个字节上，即让tail指向头一个结点
		NextObj(tail) = _freelist;
		//head是一个指向头结点的指针，赋值给另一个指针，那么这个指针也指向头结点
		_freelist = head;
	}
	size_t PopRange(void* start, void* end, size_t num)
	{
		size_t actualNum = 0;
		void* cur = _freelist;
		void*prev = nullptr;
		for (; actualNum < num && cur!=nullptr; ++actualNum)
		{
			prev = cur;
			cur = NextObj(cur);
		}

		start = _freelist;
		end = prev;
		_freelist = cur;

		return actualNum;
	}

	void* Pop()
	{
		//头删
		void* obj = _freelist;
		_freelist = NextObj(obj);
		return obj;
	}
	bool Empty()
	{
		return (_freelist == nullptr);
	}
private:
	void* _freelist= nullptr;
};

class SizeClass
{
public:
	// 控制在12%左右的内碎片浪费 
	// [1,128]                  8byte对齐  freelist[0,16) 
	// [129,1024]               16byte对齐     freelist[16,72) 
	// [1025,8*1024]            128byte对齐    freelist[72,128) 
	// [8*1024+1,64*1024]       512byte对齐  freelist[128,240)

	//根据对象大小计算指针数组下标
	static size_t _ListIndex(size_t size, int alig_shift)
	{
		//2的align_shift次幂是alignment
		return ((size + ((1 << alig_shift) - 1)) >> alig_shift) -1;
	}

	static inline size_t ListIndex(size_t size)
	{
		assert(size <= MAX_SIZE);
		static int group_array[3] = { 16, 72, 128 };
		if (size <= 128)
			return _ListIndex(size, 3);
		else if (size <= 1024)
		{
			return _ListIndex(size - 128, 4) + group_array[0];
		}
		else if (size <= 8192)
			return _ListIndex(size - 1024, 7) + group_array[1];
		else if (size <= 65536)
		return _ListIndex(size - 8192, 10) + group_array[2];

		return -1;
	}

	static size_t _RoundUp(size_t size, int alignment)
	{
		// (1-7) + 7 = (8-14) -> 8 4 2 1  8-14转换成二进制都是第四位为1，前三位自由组合，让他们对齐到8 所以&取反的7
		//（17-23）+7 =（24-30）->16 8 4 2 1 
		return (size + alignment - 1)&(~(alignment - 1));
	}
	//向上对齐 alignment 对齐数
	static inline size_t RoundUp(size_t size)
	{
		assert(size <= MAX_SIZE);
		if (size <= 128)
			return _RoundUp(size, 8);
		else if (size <= 1024)
			return _RoundUp(size, 16);
		else if (size <= 8192)
			return _RoundUp(size, 128);
		else if (size <= 65536)
			return _RoundUp(size, 1024);

		return -1;
	}
	//针对每一个size挪动Num个对象过去
	static size_t NumMoveSize(size_t size)
	{
		if (size == 0)
			return 0;
		int num = MAX_SIZE / size;

		if (num < 2)
			num = 2;

		if (num > 512)
			num = 512;
		return num;
	}

	static size_t NumMovePage(size_t size)
	{
		size_t num = NumMoveSize(size);
		size_t npage = num*size;
		npage >>= 12;
		if (npage == 0)
			npage = 1;
		return npage;
	}
};

//span 跨度：管理页为单位的内存对象，本质是方便做合并，解决内存碎片。
#ifdef _WIN32
typedef unsigned int PAGE_ID;
#else
typedef unsigned long long PAGE_ID;
#endif

struct Span
{
	PAGE_ID _pageid;//页号
	int _pagesize;//页的数量
	FreeList _freelist;//对象自由链表
	int _usecount;//内存块对象使用计数

	size_t objsize;//对象大小

	Span* _next;
	Span* _prev;
};

//SpanList是一个将Span链起来的带头节点的循环链表
class SpanList
{
public:
	SpanList()
	{
		_head = new Span;
		_head->_next = _head;
		_head->_prev = _head;
	}

	bool Empty()
	{
		return(_head == _head->_next);
	}
	Span* Begin()
	{
		return _head->_next;
	}

	Span* End()
	{
		return _head;
	}

	void PushFront(Span* newpos)
	{
		Insert(_head->_next, newpos);
	}

	void PopFront()
	{
		Erase(_head->_next);
	}

	void PushBack(Span* newpos)
	{
		Insert(_head, newpos);
	}

	void PopBack()
	{
		Erase(_head->_prev);
	}

	void Insert(Span* pos, Span* newspan)
	{
		Span* prev = pos->_prev;
		//prev newspan pos
		prev->_next = newspan;
		newspan->_next = pos;
		newspan->_prev = prev;
		pos->_prev = newspan;
	}

	void Erase(Span* pos)
	{
		assert(pos != _head);
		Span* prev = pos->_prev;
		Span* next = pos->_next;
		prev->_next = next;
		next->_prev = prev;
	}
private:
	Span* _head;
};

//向系统申请numpage页内存挂到自由链表
inline static void* SystemAllocPage(size_t num_page)
{
#ifdef _WIN32
	void* ptr = VirtualAlloc(0, num_page*(1 << PAGE_SHIFT),
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	//brk mmap等
#endif
	if (ptr == nullptr)
		throw std::bad_alloc();

	return ptr;
}