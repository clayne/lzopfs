#ifndef LRUMAP_H
#define LRUMAP_H

#include "TR1.h"

#include <list>

template <
	typename Key,
	typename Value,
	typename Hash = hash<Key> >
class LRUMap {
public:
	typedef size_t Weight;
	
	struct Entry {
		Key key;
		Value value;
		Weight weight;
		
		Entry(Key k, Value v, Weight w) : key(k), value(v), weight(w) { }
	};
	
	struct OverWeight : std::runtime_error {
		OverWeight() : std::runtime_error("LRUMap element too large") { }
	};

private:
	typedef std::list<Entry> LRUList; // most-recent at front
	typedef typename LRUList::iterator LRUIterator;
	typedef unordered_map<Key, LRUIterator, Hash> IterMap;

public:
	typedef LRUIterator Iterator;

private:	
	LRUList mLRU;
	IterMap mMap;
	Weight mWeight, mMaxWeight;
	
	void makeRoom(Weight newWeight) {
		while (!mLRU.empty() && mWeight > newWeight) {
			Entry &e = mLRU.back();
			mMap.erase(e.key);
			mWeight -= e.weight;
			mLRU.pop_back();
		}
	}
	
	void markNew(const LRUIterator& iter) {
		mLRU.splice(mLRU.begin(), mLRU, iter);
	}
	
public:
	LRUMap(Weight maxWeight) : mWeight(), mMaxWeight(maxWeight) { }
	
	Weight weight() const { return mWeight; }
	
	Weight maxWeight() const { return mMaxWeight; }
	void maxWeight(Weight w) { makeRoom(w); mMaxWeight = w; }
	
	// Doesn't change LRU-time
	Iterator begin() { return mLRU.begin(); }
	Iterator end() { return mLRU.end(); }
	
	// Add a new item, ejecting old items to make room if necessary
	Entry& add(const Key& k, const Value& v, Weight w) {
		typename IterMap::iterator miter = mMap.find(k);
		if (miter != mMap.end()) {
			// Don't allow duplicates, just consider this a read
			markNew(miter->second);
			return *(miter->second);
		}

		if (w > maxWeight())
			throw OverWeight();
		
		makeRoom(maxWeight() - w);
		mWeight += w;
		mLRU.push_front(Entry(k, v, w));
		mMap[k] = mLRU.begin();
		return mLRU.front();
	}
	
	// Find an item, returning null-ptr if not found
	Value *find(const Key& k) {
		typename IterMap::iterator miter = mMap.find(k);
		if (miter == mMap.end())
			return 0;
		
		markNew(miter->second);
		return &miter->second->value;
	}
};

#endif // LRUMAP_H
