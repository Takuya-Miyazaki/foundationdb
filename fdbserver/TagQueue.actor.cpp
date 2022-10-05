#include "fdbserver/TagQueue.h"
#include "flow/UnitTest.h"
#include "flow/actorcompiler.h" // must be last include

void TagQueue::updateRates(std::map<TransactionTag, double> const& newRates) {
	for (const auto& [tag, rate] : newRates) {
		auto it = rateInfos.find(tag);
		if (it == rateInfos.end()) {
			rateInfos[tag] = GrvTransactionRateInfo(rate);
		} else {
			it->second.setRate(rate);
		}
	}

	for (const auto& [tag, _] : rateInfos) {
		if (newRates.find(tag) == newRates.end()) {
			rateInfos.erase(tag);
		}
	}
}

bool TagQueue::canStart(TransactionTag tag, int64_t count) const {
	auto it = rateInfos.find(tag);
	if (it == rateInfos.end()) {
		return true;
	}
	auto it2 = releasedInEpoch.find(tag);
	auto alreadyReleased = (it2 == releasedInEpoch.end() ? 0 : it2->second);
	return it->second.canStart(alreadyReleased, count);
}

bool TagQueue::canStart(GetReadVersionRequest req) const {
	if (req.priority == TransactionPriority::IMMEDIATE) {
		return true;
	}
	for (const auto& [tag, count] : req.tags) {
		if (!canStart(tag, count)) {
			return false;
		}
	}
	return true;
}

void TagQueue::addRequest(GetReadVersionRequest req) {
	newRequests.push_back(req);
}

void TagQueue::startEpoch() {
	for (auto& [_, rateInfo] : rateInfos) {
		rateInfo.startEpoch();
	}
	releasedInEpoch.clear();
}

void TagQueue::endEpoch(double elapsed) {
	for (auto& [tag, rateInfo] : rateInfos) {
		rateInfo.endEpoch(releasedInEpoch[tag], false, elapsed);
	}
}

void TagQueue::runEpoch(double elapsed,
                        SpannedDeque<GetReadVersionRequest>& outBatchPriority,
                        SpannedDeque<GetReadVersionRequest>& outDefaultPriority,
                        SpannedDeque<GetReadVersionRequest>& outImmediatePriority) {
	startEpoch();
	Deque<DelayedRequest> newDelayedRequests;

	while (!delayedRequests.empty()) {
		auto const& delayedReq = delayedRequests.front();
		auto const& req = delayedReq.req;
		if (canStart(req)) {
			for (const auto& [tag, count] : req.tags) {
				releasedInEpoch[tag] += count;
			}
			if (req.priority == TransactionPriority::BATCH) {
				outBatchPriority.push_back(req);
			} else if (req.priority == TransactionPriority::DEFAULT) {
				outDefaultPriority.push_back(req);
			} else if (req.priority == TransactionPriority::IMMEDIATE) {
				outImmediatePriority.push_back(req);
			} else {
				ASSERT(false);
			}
		} else {
			newDelayedRequests.push_back(delayedReq);
		}
		delayedRequests.pop_front();
	}

	while (!newRequests.empty()) {
		auto const& req = newRequests.front();
		if (canStart(req)) {
			for (const auto& [tag, count] : req.tags) {
				releasedInEpoch[tag] += count;
			}
			if (req.priority == TransactionPriority::BATCH) {
				outBatchPriority.push_back(req);
			} else if (req.priority == TransactionPriority::DEFAULT) {
				outDefaultPriority.push_back(req);
			} else if (req.priority == TransactionPriority::IMMEDIATE) {
				outImmediatePriority.push_back(req);
			} else {
				ASSERT(false);
			}
		} else {
			newDelayedRequests.emplace_back(req);
		}
		newRequests.pop_front();
	}

	delayedRequests = std::move(newDelayedRequests);
	endEpoch(elapsed);
}

ACTOR static Future<Void> mockClient(TagQueue* tagQueue,
                                     TransactionPriority priority,
                                     TagSet tagSet,
                                     int batchSize,
                                     double desiredRate,
                                     TransactionTagMap<uint32_t>* counters) {
	state Future<Void> timer;
	state TransactionTagMap<uint32_t> tags;
	for (const auto& tag : tagSet) {
		tags[tag] = batchSize;
	}
	loop {
		timer = delayJittered(static_cast<double>(batchSize) / desiredRate);
		GetReadVersionRequest req;
		req.tags = tags;
		req.priority = priority;
		tagQueue->addRequest(req);
		wait(success(req.reply.getFuture()) && timer);
		for (auto& [tag, _] : tags) {
			(*counters)[tag] += batchSize;
		}
	}
}

ACTOR static Future<Void> mockServer(TagQueue* tagQueue) {
	state SpannedDeque<GetReadVersionRequest> outBatchPriority("TestTagQueue_Batch"_loc);
	state SpannedDeque<GetReadVersionRequest> outDefaultPriority("TestTagQueue_Default"_loc);
	state SpannedDeque<GetReadVersionRequest> outImmediatePriority("TestTagQueue_Immediate"_loc);
	loop {
		state double elapsed = (0.009 + 0.002 * deterministicRandom()->random01());
		wait(delay(elapsed));
		tagQueue->runEpoch(elapsed, outBatchPriority, outDefaultPriority, outImmediatePriority);
		while (!outBatchPriority.empty()) {
			outBatchPriority.front().reply.send(GetReadVersionReply{});
			outBatchPriority.pop_front();
		}
		while (!outDefaultPriority.empty()) {
			outDefaultPriority.front().reply.send(GetReadVersionReply{});
			outDefaultPriority.pop_front();
		}
		while (!outImmediatePriority.empty()) {
			outImmediatePriority.front().reply.send(GetReadVersionReply{});
			outImmediatePriority.pop_front();
		}
	}
}

static bool isNear(double desired, int64_t actual) {
	return std::abs(desired - actual) * 10 < desired;
}

// Rate limit set at 10, but client attempts 20 transactions per second.
// Client should be throttled to only 10 transactions per second.
TEST_CASE("/TagQueue/Simple") {
	state TagQueue tagQueue;
	state TagSet tagSet;
	state TransactionTagMap<uint32_t> counters;
	{
		std::map<TransactionTag, double> rates;
		rates["sampleTag"_sr] = 10.0;
		tagQueue.updateRates(rates);
	}
	tagSet.addTag("sampleTag"_sr);

	state Future<Void> client = mockClient(&tagQueue, TransactionPriority::DEFAULT, tagSet, 1, 20.0, &counters);
	state Future<Void> server = mockServer(&tagQueue);
	wait(timeout(client && server, 60.0, Void()));
	TraceEvent("TagQuotaTest_Simple").detail("Counter", counters["sampleTag"_sr]);
	ASSERT(isNear(counters["sampleTag"_sr], 60.0 * 10.0));
	return Void();
}

// Immediate-priority transactions are not throttled by the TagQueue
TEST_CASE("/TagQueue/Immediate") {
	state TagQueue tagQueue;
	state TagSet tagSet;
	state TransactionTagMap<uint32_t> counters;
	{
		std::map<TransactionTag, double> rates;
		rates["sampleTag"_sr] = 10.0;
		tagQueue.updateRates(rates);
	}
	tagSet.addTag("sampleTag"_sr);

	state Future<Void> client = mockClient(&tagQueue, TransactionPriority::IMMEDIATE, tagSet, 1, 20.0, &counters);
	state Future<Void> server = mockServer(&tagQueue);
	wait(timeout(client && server, 60.0, Void()));
	TraceEvent("TagQuotaTest_Immediate").detail("Counter", counters["sampleTag"_sr]);
	ASSERT(isNear(counters["sampleTag"_sr], 60.0 * 20.0));
	return Void();
}

// Throttle based on the tag with the lowest rate
TEST_CASE("/TagQueue/MultiTag") {
	state TagQueue tagQueue;
	state TagSet tagSet;
	state TransactionTagMap<uint32_t> counters;
	{
		std::map<TransactionTag, double> rates;
		rates["sampleTag1"_sr] = 10.0;
		rates["sampleTag2"_sr] = 20.0;
		tagQueue.updateRates(rates);
	}
	tagSet.addTag("sampleTag1"_sr);
	tagSet.addTag("sampleTag2"_sr);

	state Future<Void> client = mockClient(&tagQueue, TransactionPriority::DEFAULT, tagSet, 1, 30.0, &counters);
	state Future<Void> server = mockServer(&tagQueue);
	wait(timeout(client && server, 60.0, Void()));
	TraceEvent("TagQuotaTest_MultiTag").detail("Counter", counters["sampleTag1"_sr]);
	ASSERT_EQ(counters["sampleTag1"_sr], counters["sampleTag2"_sr]);
	ASSERT(isNear(counters["sampleTag1"_sr], 60.0 * 10.0));

	return Void();
}

// Clients share the available 10 transaction/second budget
TEST_CASE("/TagQueue/MultiClient") {
	state TagQueue tagQueue;
	state TagSet tagSet;
	state TransactionTagMap<uint32_t> counters;
	{
		std::map<TransactionTag, double> rates;
		rates["sampleTag"_sr] = 10.0;
		tagQueue.updateRates(rates);
	}
	tagSet.addTag("sampleTag"_sr);

	state Future<Void> client1 = mockClient(&tagQueue, TransactionPriority::DEFAULT, tagSet, 1, 20.0, &counters);
	state Future<Void> client2 = mockClient(&tagQueue, TransactionPriority::DEFAULT, tagSet, 1, 20.0, &counters);

	state Future<Void> server = mockServer(&tagQueue);
	wait(timeout(client1 && client2 && server, 60.0, Void()));
	TraceEvent("TagQuotaTest_MultiClient").detail("Counter", counters["sampleTag"_sr]);
	ASSERT(isNear(counters["sampleTag"_sr], 60.0 * 10.0));
	return Void();
}

TEST_CASE("/TagQueue/Batch") {
	state TagQueue tagQueue;
	state TagSet tagSet;
	state TransactionTagMap<uint32_t> counters;
	{
		std::map<TransactionTag, double> rates;
		rates["sampleTag"_sr] = 10.0;
		tagQueue.updateRates(rates);
	}
	tagSet.addTag("sampleTag"_sr);

	state Future<Void> client = mockClient(&tagQueue, TransactionPriority::DEFAULT, tagSet, 5, 20.0, &counters);
	state Future<Void> server = mockServer(&tagQueue);
	wait(timeout(client && server, 60.0, Void()));

	TraceEvent("TagQuotaTest_Batch").detail("Counter", counters["sampleTag"_sr]);
	ASSERT(isNear(counters["sampleTag"_sr], 60.0 * 10.0));
	return Void();
}
