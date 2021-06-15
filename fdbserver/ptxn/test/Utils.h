/*
 * Utils.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2021 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDBSERVER_PTXN_TEST_UTILS_H
#define FDBSERVER_PTXN_TEST_UTILS_H

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "fdbserver/ptxn/test/Driver.h"
#include "fdbserver/ptxn/test/TestTLogPeek.h"
#include "fdbserver/ptxn/TLogInterface.h"

namespace ptxn::test {

// Shortcut for deterministicRanodm()->randomUniqueID();
UID randomUID();

// Constructs a random StorageTeamID
StorageTeamID getNewStorageTeamID();

// Construct numStorageTeams TeamIDs
std::vector<StorageTeamID> generateRandomStorageTeamIDs(const int numStorageTeams);

// Pick one element from a container, randomly
// The container should support the concept of
//  template <typename Container>
//  requires(const Container& container) {
//      { container[0] };
//      { container.size() };
//      { Container::const_reference }
//  }
template <typename Container>
typename Container::const_reference randomlyPick(const Container& container) {
	if (container.size() == 0) {
		throw std::range_error("empty container");
	}
	return container[deterministicRandom()->randomInt(0, container.size())];
};

namespace print {

void print(const TLogCommitRequest&);
void print(const TLogCommitReply&);
void print(const TLogPeekRequest&);
void print(const TLogPeekReply&);
void print(const TestDriverOptions&);
void print(const CommitRecord&);
void print(const ptxn::test::TestTLogPeekOptions&);

void printCommitRecords(const std::vector<CommitRecord>&);
void printNotValidatedRecords(const std::vector<CommitRecord>&);

// Prints timing per step
class PrintTiming {

	using time_point_t = std::chrono::time_point<std::chrono::high_resolution_clock>;
	using clock_t = std::chrono::high_resolution_clock;
	using duration_t = std::chrono::duration<double>;

	std::string functionName;
	time_point_t startTime;
	time_point_t lastTagTime;

	class DummyOStream {};

public:
	// Helper class print out time and duration
	PrintTiming(const std::string&);
	~PrintTiming();

	template <typename T>
	friend DummyOStream operator<<(PrintTiming&, const T& object);

	// Support iomanips like std::endl
	// Here we *MUST* use pointers instead of std::function, since the  see, e.g.:
	//   * http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0551r3.pdf
	friend DummyOStream operator<<(PrintTiming&, std::ostream&(*)(std::ostream&));

	friend DummyOStream operator<<(PrintTiming&, std::ios_base&(*)(std::ios_base&));

	template <typename T>
	friend DummyOStream&& operator<<(DummyOStream&&, const T& object);

	friend DummyOStream&& operator<<(DummyOStream&&, std::ostream&(*)(std::ostream&));

	friend DummyOStream&& operator<<(DummyOStream&&, std::ios_base&(*)(std::ios_base&));
};

template <typename T>
PrintTiming::DummyOStream operator<<(PrintTiming& printTiming, const T& object) {
	auto now = PrintTiming::clock_t::now();
	std::cout << std::setw(25) << printTiming.functionName << ">> "
	          << "[" << std::setw(12) << std::fixed << std::setprecision(6)
	          << PrintTiming::duration_t(now - printTiming.startTime).count() << "] ";
	std::cout << object;
	printTiming.lastTagTime = now;

    return PrintTiming::DummyOStream();
}

template <typename T>
PrintTiming::DummyOStream&& operator<<(PrintTiming::DummyOStream&& stream, const T& object) {
	std::cout << object;
	return std::move(stream);
}

} // namespace print

} // namespace ptxn::test

#endif // FDBSERVER_PTXN_TEST_UTILS_H