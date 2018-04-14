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

#include <csignal>
#include <sys/time.h>
#include <cerrno>

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

void RaftServer::AlarmCallback(int signum) {
  BecomeCandidate();
  return;
}

void RaftServer::SetAlarm(int after_ms) {
  struct itimerval timer;

  /* Configure timer intervals */
  timer.it_value.tv_sec = 0;
  timer.it_value.tv_usec = after_ms * 1000; // microseconds

  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = after_ms * 1000; // microseconds

  /* Set timer callback */
  int old_errno = errno;
  errno = 0;
  signal(SIGALRM, (void(*)(int))&RaftServer::AlarmCallback);
  if(errno) {
    std::perror("Error occurred while trying to set signal handler: ");
    errno = old_errno;
    return;
  }
  errno = old_errno;

  /* Set timer */
  old_errno = errno;
  errno = 0;
  setitimer(ITIMER_REAL, &timer, nullptr);
  if(errno) {
    std::perror("An error occurred while trying to set the timer: ");
  }
  errno = old_errno;
  return;
}

void RaftServer::ResetElectionTimeout() {
  struct itimerval old_timer;
  struct itimerval new_timer;

  /* Get old timer to reset to old after_ms value */
  int old_errno = errno;
  errno = 0;
  getitimer(ITIMER_REAL, &old_timer);
  if(errno) {
    std::perror("Error occurred getting old timer: ");
    errno = old_errno;
    return;
  }
  errno = old_errno;

  /* Configure timer intervals
   * The old_timer.it_value has the time remaining so far, so we don't use it.
   * The old_timer.it_interval has the interval time, and since our timers are
   * not one-shot, this interval is set and constant, so we can use this.
   */
  new_timer.it_value.tv_sec = old_timer.it_interval.tv_sec;
  new_timer.it_value.tv_usec = old_timer.it_interval.tv_usec;

  new_timer.it_interval.tv_sec = old_timer.it_interval.tv_sec;
  new_timer.it_interval.tv_usec = old_timer.it_interval.tv_usec;

  /* Set timer */
  old_errno = errno;
  errno = 0;
  setitimer(ITIMER_REAL, &new_timer, &old_timer);
  if(errno) {
    std::perror("Error occurred trying to reset timer: ");
  }
  errno = old_errno;
  return;
}
