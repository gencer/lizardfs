#pragma once

#include "common/platform.h"

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>

#include "common/hashfn.h"
#include "common/massert.h"
#include "common/time_utils.h"

/**
 * A map caching 'Values' for 'Keys' in LRU manner.
 *
 * 'KeysTupleToTimeAndValue' is an internal map type used by the cache. For details have a look
 * at example usages. If possible use 'LruCache' typedef instead.
 */
template <class Value, class KeysTupleToTimeAndValue, class Mutex, class... Keys>
class LruCacheBase {
public:
	typedef std::function<Value(Keys...)> ValueObtainer;

	/**
	 * \param maxTime period after which every cache entry is discarded
	 * \param maxElements capacity of the cache, after exceeding which values added least recently
	 *                are removed
	 * \param valueObtainer a function that is used for obatining a value for a given key, if
	 *                   cached value was not available.
	 */
	LruCacheBase(SteadyDuration maxTime, uint64_t maxElements, ValueObtainer valueObtainer)
		: maxTime_(maxTime), maxElements_(maxElements), valueObtainer_(valueObtainer) {
	}

	/**
	 * If available return value from cache. Otherwise obtain it with use of 'valueObtainer',
	 * fill the cache and return the value. If new cache entry was added, try to cleanup the whole
	 * cache a bit by removing few outdated entries if there were any, or remove the oldest entry
	 * if the capacity was exceeded.
	 */
	Value get(SteadyTimePoint currentTs, Keys... keys) {
		std::unique_lock<Mutex> lock(mutex_);
		auto keyTuple = std::make_tuple(keys...);
		auto iterator = keysToTimeAndValue_.find(keyTuple);
		if (iterator != keysToTimeAndValue_.end()) {
			auto keyTuplePointer = &iterator->first;
			auto& ts = std::get<0>(iterator->second);
			if (ts + maxTime_ >= currentTs) {
				auto& value = std::get<1>(iterator->second);
				return value;
			} else {
				auto tsAndKeys = std::make_pair(ts, keyTuplePointer);
				sassert(timeToKeys_.erase(tsAndKeys) == 1);
				keysToTimeAndValue_.erase(keyTuple);
			}
		}

		// Don't call valueObtainer under a lock
		lock.unlock();
		auto value = valueObtainer_(keys...);
		lock.lock();
		// If there was a race and the cache was filled after the lock was released, return the
		// value that was just obtained, don't update the cache itself.
		iterator = keysToTimeAndValue_.find(keyTuple);
		if (iterator != keysToTimeAndValue_.end()) {
			return value;
		}

		try {
			auto tsAndValue = std::make_pair(currentTs, std::move(value));
			keysToTimeAndValue_.insert(std::make_pair(keyTuple, std::move(tsAndValue)));
			auto keyToTimeAndValue = keysToTimeAndValue_.find(keyTuple);
			auto keysTuplePointer = &keyToTimeAndValue->first;
			auto tsAndKeys = std::make_pair(currentTs, keysTuplePointer);
			timeToKeys_.insert(tsAndKeys);
		} catch (...) {
			keysToTimeAndValue_.erase(keyTuple);
			throw;
		}

		// If one value was (possibly) added, remove a few values, to keep
		// number of elements in the cache limited
		uint64_t few = 3;
		this->cleanupWithoutLocking(currentTs, few);

		return value;
	}

	/**
	 * Remove 'maxOperations' or less entries that are either outdated or make the cache exceed
	 * its capacity.
	 *
	 * Due to the way 'get' method was implemented the cache should never exceed its limit by more
	 * then 1, so this function does not have to be called by hand.
	 */
	void cleanup(SteadyTimePoint currentTs, uint64_t maxOperations) {
		std::unique_lock<Mutex> lock(mutex_);
		cleanupWithoutLocking(currentTs, maxOperations);
	}

	/**
	 * Erase a cache entry, if there was one matching the key. Otherwise silently return.
	 */
	void erase(Keys... keys) {
		std::unique_lock<Mutex> lock(mutex_);
		auto keyTuple = std::make_tuple(keys...);
		auto iterator = keysToTimeAndValue_.find(keyTuple);
		if (iterator != keysToTimeAndValue_.end()) {
			auto keyTuplePointer = &iterator->first;
			TimeAndValue& timeAndValue = iterator->second;
			auto ts = timeAndValue.first;
			auto tsAndKeys = std::make_pair(ts, keyTuplePointer);
			sassert(timeToKeys_.erase(tsAndKeys) == 1);
			keysToTimeAndValue_.erase(iterator);
		}
	}

private:
	/**
	 * Does the same as 'cleanup', but without acquiring a lock
	 */
	void cleanupWithoutLocking(SteadyTimePoint currentTs, uint64_t maxOperations) {
		for (uint64_t i = 0; i < maxOperations; ++i) {
			auto oldestEntry = timeToKeys_.begin();
			if (oldestEntry == timeToKeys_.end()) {
				return;
			}
			auto& ts = std::get<0>(*oldestEntry);
			if ((ts + maxTime_ < currentTs) || (timeToKeys_.size() > maxElements_)) {
				auto keyTuplePtr = std::get<1>(*oldestEntry);
				timeToKeys_.erase(oldestEntry);
				sassert(keysToTimeAndValue_.erase(*keyTuplePtr) == 1);
			} else {
				return;
			}
		}
	}

	SteadyDuration maxTime_;
	uint64_t maxElements_;
	ValueObtainer valueObtainer_;
	Mutex mutex_;

	typedef std::tuple<Keys...> KeysTuple;
	typedef std::pair<SteadyTimePoint, Value> TimeAndValue;
	typedef std::pair<SteadyTimePoint, const KeysTuple*> TimeAndKeysPtr;

	KeysTupleToTimeAndValue keysToTimeAndValue_;
	std::set<TimeAndKeysPtr> timeToKeys_;
};

/**
 * Mutex that does nothing, to be used when synchronization is not needed
 */
struct SingleThreadMutex {
	void lock() {
	}
	void unlock() {
	}
};

template <class Value, class... Keys>
using TreeLruCache = LruCacheBase<
		Value,
		std::map<
				std::tuple<Keys...>,
				std::pair<SteadyTimePoint, Value>>,
		SingleThreadMutex,
		Keys...>;

template <class Value, class... Keys>
using TreeLruCacheMt = LruCacheBase<
		Value,
		std::map<
				std::tuple<Keys...>,
				std::pair<SteadyTimePoint, Value>>,
		std::mutex,
		Keys...>;

template <class Value, class... Keys>
using LruCache = LruCacheBase<
		Value,
		std::unordered_map<
				std::tuple<Keys...>,
				std::pair<SteadyTimePoint, Value>,
				AlmostGenericTupleHash<Keys...>>,
		SingleThreadMutex,
		Keys...>;

template <class Value, class... Keys>
using LruCacheMt = LruCacheBase<
		Value,
		std::unordered_map<
				std::tuple<Keys...>,
				std::pair<SteadyTimePoint, Value>,
				AlmostGenericTupleHash<Keys...>>,
		std::mutex,
		Keys...>;

