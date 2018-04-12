#include "RaftServer.h"
#include <grpc/grpc.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/security/server_credentials.h>
#include <grpc++/create_channel.h>
#include "raft.grpc.pb.h"

#include <utility>
#include <fstream>
#include <memory>

using std::string;
using grpc::Status;
using grpc::ServerContext;
using raft::AppendEntriesRequest;
using raft::AppendEntriesResponse;
using raft::RequestVoteRequest;
using raft::RequestVoteResponse;

RaftServer::RaftServer(int o_id, const std::vector<string>& hostList,
                       const string& storageDir,
                       std::unique_ptr<StateMachineInterface> o_stateMachine):
id(o_id),
stateMachine(std::move(o_stateMachine)),
mustBecomeCandidate(false),
hostCount(hostList.size()),
commitIndex(-1),
lastApplied(-1),
currentLeader(-1),
serverState(ServerState::Follower),
storage(storageDir) {
  for (int i = 0; i < hostCount; i++) {
    if (i == id) {
      stubs.push_back(nullptr);
    } else {
      stubs.emplace_back(Raft::NewStub(
        grpc::CreateChannel(hostList[i], grpc::InsecureChannelCredentials())));
    }
  }
}

void RaftServer::BecomeFollower() {
  serverState = ServerState::Follower;
}

void RaftServer::BecomeCandidate() {
  mustBecomeCandidate = true;
  std::lock_guard<std::mutex> lock(overallLock);
  if (!mustBecomeCandidate) {
    return;
  }

  serverState = ServerState::Candidate;
  currentTerm++;
  votedFor = id;
  // storageImpl->Update(currentTerm, votedFor, log);
  // alarmService->ResetElectionTimeout(PickElectionTimeout());

  for(int i = 0; i < hostCount; i++) {
    if(i != id) {
      // requestImpl->RequestVote(
      //   i,
      //   currentTerm,
      //   log.size() - 1,
      //   log.back().term);
    }
  }
}

void RaftServer::RequestVoteCallback(int responseTerm, bool voteGranted) {
  std::lock_guard<std::mutex> lock(overallLock);
  if (responseTerm > currentTerm) {
    currentTerm = responseTerm;
    BecomeFollower();
    return;
  }

  if (voteGranted) {
    votesGained++;
  }
  if (votesGained > hostCount/2) { 
    // BecomeLeader();
  }
}

Status RaftServer::AppendEntries(ServerContext* context,
                                   const AppendEntriesRequest* request,
                                   AppendEntriesResponse *response) {
  std::lock_guard<std::mutex> lock(overallLock);

  response->set_term(currentTerm);
  response->set_success(false);
  
  if (request->term() < currentTerm) {
    return Status::OK;
  } else {
	  // request->term() >= currentTerm
    currentTerm = request->term();
    BecomeFollower();
  }

  if (request->prevlogindex() >= log.size() ||
      log[request->prevlogindex()].term() != request->prevlogterm()) {
    return Status::OK;
  }

  uint curLogIndex = request->prevlogindex() + 1;
  bool mustRecreateLog = false;
  int startIndex = 0;
  for (string entry: request->entries()) {
    if (log[curLogIndex].term() != request->term()) {
      log.resize(curLogIndex);
      mustRecreateLog = true;
      startIndex = curLogIndex;
    }
    if (curLogIndex >= log.size()) {
      LogEntry new_entry;
      new_entry.set_term(request->term());
      new_entry.set_command(entry);
      log.push_back(new_entry);
    }
    curLogIndex++;
  }
  storage.UpdateState(currentTerm, votedFor);
  storage.UpdateLog(log.cbegin() + startIndex, log.cend(), !mustRecreateLog);

  if(request->leadercommit() > commitIndex) {
    commitIndex = std::min(request->leadercommit(), int(log.size() - 1));
    while(lastApplied < commitIndex) {
      lastApplied++;
      stateMachine->Apply(log[lastApplied].command());
    }
  }

  currentLeader = request->leaderid();
  response->set_success(true);

  return Status::OK;
}

Status RaftServer::RequestVote(ServerContext* context,
                             const RequestVoteRequest* request,
                             RequestVoteResponse* response) {
  std::lock_guard<std::mutex> lock(overallLock);

  response->set_term(currentTerm);
  response->set_votegranted(false);

  if(request->term() < currentTerm) {
    return Status::OK;
  }

  if (request->term() > currentTerm) {
    currentTerm = request->term();
    BecomeFollower();
  }

  if(votedFor != -1 && votedFor != request->candidateid()) {
    return Status::OK;
  }

  if (log.back().term() > request->lastlogterm() ||
     (log.back().term() == request->lastlogterm() &&
      log.size() - 1 > request->lastlogindex())) {
    return Status::OK;
  }

  ResetElectionTimeout();
  storage.UpdateState(currentTerm, votedFor);
  votedFor = request->candidateid();
  response->set_votegranted(true);

  return Status::OK;
}
