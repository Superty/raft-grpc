#include "RaftServer.h"

#include <utility>
#include <fstream>

using std::string;
using std::unique_ptr;

RaftServerImpl::RaftServerImpl(
  int id,
  unique_ptr<RequestInterface> o_requestImpl,
  unique_ptr<ResponseInterface> o_responseImpl,
  unique_ptr<StateMachineInterface> o_stateMachine,
  unique_ptr<StorageInterface> o_storageImpl,
  unique_ptr<AlarmServiceInteface> o_alarmService,
  const std::vector<string>& hostList,
  const string& logFile
):
myId(id),
requestImpl(std::move(o_requestImpl)),
responseImpl(std::move(o_responseImpl)),
stateMachine(std::move(o_stateMachine)),
storageImpl(std::move(o_storageImpl)),
alarmService(std::move(o_alarmService)),
mustBecomeCandidate(false),
hostCount(hostList.size()),
commitIndex(-1),
lastApplied(-1),
currentLeader(-1),
serverState(ServerState::Follower)
{
  requestImpl->Initialize(hostList, this);
  responseImpl->Initialize(this);
  storageImpl->Initialize(logFile);
  storageImpl->InitializeState(&currentTerm, &votedFor, &log);

  distribution = std::uniform_int_distribution<int>(
    minElectionTimeout,
    maxElectionTimeout
  );
  pickElectionTimeout = std::bind(distribution, generator);

  Run();
}

// Called from this object
void RaftServerImpl::BecomeFollower() {
  serverState = ServerState::Follower;
}

// Called by the election alarm (by alarmService)
void RaftServerImpl::BecomeCandidate() {
  mustBecomeCandidate = true;
  std::lock_guard<std::mutex> lock(overallLock);
  if (not mustBecomeCandidate) {
    return;
  }

  serverState = ServerState::Candidate;
  currentTerm++;
  votedFor = myId;
  storageImpl->Update();
  alarmService->ResetElectionTimeout(PickElectionTimeout());

  for(int id = 0; id < hostCount; id++) {
    if(id != myId) {
      requestImpl->RequestVote(
        id,
        currentTerm,
        myId,
        log.size() - 1,
        log.back().term);
    }
  }
}

// Called by requestImpl
void RaftServerImpl::RequestVoteCallback(int responseTerm, bool voteGranted) {
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
    BecomeLeader();
  }
}

// Called by responseImpl
void RaftServerImpl::HandleAppendEntries(
  int leaderTerm,
  int leaderId,
  int prevLogIndex,
  int prevLogTerm,
  const std::vector<LogEntry>& entries,
  int leaderCommit,
  int * responseTerm,
  bool * success
) {
  std::lock_guard<std::mutex> lock(overallLock);

  bool mustUpdateStorage = false;
  *responseTerm = currentTerm;

  if (leaderTerm < currentTerm) {
    *success = false;
    return;
  }

  if (leaderTerm > currentTerm) {
    currentTerm = leaderTerm;
    BecomeFollower();
    mustUpdateStorage = true;
  }
  else if (serverState == ServerState::Candidate) {
    // note: if we're here, leaderTerm == currentTerm
    BecomeFollower();
  }

  if (prevLogIndex >= log.size()
           || log[prevLogIndex].term != prevLogTerm) {
    *success = false;
    return;
  }

  uint curLogIndex = prevLogIndex + 1;
  bool mustAppend = false;
  for (LogEntry entry: entries) {
    if (mustAppend) {
      log.push_back(entry);
      mustUpdateStorage = true;
    }
    else if (curLogIndex >= log.size()) {
      mustAppend = true;
    }
    else if (log[curLogIndex].term != entry.term) {
      log.resize(curLogIndex);
      mustUpdateStorage = true;
      mustAppend = true;
    }
    curLogIndex++;
  }

  if(leaderCommit > commitIndex) {
    commitIndex = std::min(leaderCommit, int(log.size() - 1));
    while(lastApplied < commitIndex) {
      lastApplied++;
      stateMachine->Process(log[lastApplied].command);
    }
  }

  if(mustUpdateStorage) {
    storageImpl->Update(currentTerm, votedFor, log);
  }

  currentLeader = leaderId;
  mustBecomeCandidate = false;
  *success = true;
}

// called by responseImpl
void RaftServerImpl::HandleRequestVote(
  int candidateTerm,
  int candidateId,
  int lastLogIndex,
  int lastLogTerm,
  int * responseTerm,
  bool * voteGranted
) {
  std::lock_guard<std::mutex> lock(overallLock);

  *responseTerm = currentTerm;

  if(candidateTerm < currentTerm) {
    *voteGranted = false;
    return;
  }

  if (candidateTerm > currentTerm) {
    currentTerm = candidateTerm;
    BecomeFollower();
  }

  if(votedFor != -1 && votedFor != candidateId) {
    *voteGranted = false;
    return;
  }

  if (log.back().term > lastLogTerm ||
     (log.back().term == lastLogTerm && log.size() - 1 > lastLogIndex)) {
    *voteGranted = false;
    return;
  }

  votedFor = candidateId;
  storageImpl->Update(currentTerm, votedFor, log);
  *voteGranted = true;
  mustBecomeCandidate = false;
}
