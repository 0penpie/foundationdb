/*
 * FakeTLog.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
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

#if defined(NO_INTELLISENSE) && !defined(FDBSERVER_TEAM_TEST_FAKETLOG_ACTOR_G_H)
#define FDBSERVER_TEAM_TEST_FAKETLOG_ACTOR_G_H
#include "fdbserver/ptxn/test/FakeTLog.actor.g.h"
#elif !defined(FDBSERVER_TEAM_TEST_FAKETLOG_ACTOR_H)
#define FDBSERVER_TEAM_TEST_FAKETLOG_ACTOR_H

#include <memory>
#include <unordered_map>

#include "fdbclient/FDBTypes.h"
#include "fdbserver/ptxn/test/Driver.h"
#include "fdbserver/ptxn/TLogInterface.h"
#include "fdbserver/ptxn/StorageServerInterface.h"
#include "flow/flow.h"

#include "flow/actorcompiler.h" // has to be last include

#pragma once

namespace ptxn {

struct FakeTLogContext {
	std::shared_ptr<TestDriverContext> pTestDriverContext;
	std::shared_ptr<TLogInterfaceBase> pTLogInterface;
};

ACTOR Future<Void> fakeTLog_ActivelyPush(std::shared_ptr<FakeTLogContext> pFakeTLogContext);
ACTOR Future<Void> fakeTLog_PassivelyProvide(std::shared_ptr<FakeTLogContext> pFakeTLogContext);

Future<Void> getFakeTLogActor(const MessageTransferModel model, std::shared_ptr<FakeTLogContext> pFakeTLogContext);

} // namespace ptxn

#include "flow/unactorcompiler.h"
#endif // FDBSERVER_TEAM_TEST_FAKETLOG_ACTOR_H
