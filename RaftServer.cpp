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
#include <cstdlib>
#include <chrono>
#include <thread>
#include <atomic>

#include <csignal>
#include <sys/time.h>
#include <cerrno>

using std::string;
using grpc::Status;
using grpc::ServerContext;
using grpc::CompletionQueue;
using grpc::ClientContext;
using grpc::ServerBuilder;
using grpc::ClientAsyncResponseReader;
using raft::AppendEntriesRequest;
using raft::AppendEntriesResponse;
using raft::RequestVoteRequest;
using raft::RequestVoteResponse;
using raft::ServeClientRequest;
using raft::ServeClientResponse;

const int RaftServer::minElectionTimeout = 800;
const int RaftServer::maxElectionTimeout = 1600;
const int RaftServer::heartbeatInterval = 50;

RaftServer* alarmHandlerServer;
void HandleSignal(int signum) {
  alarmHandlerServer->AlarmCallback();
}

void NoopStateMachine::Apply(const string& command) { return; }

RaftServer::RaftServer(int o_id, const std::vector<string>& o_hostList,
                       const string& storageDir,
                       std::unique_ptr<StateMachineInterface> o_stateMachine):
id(o_id),
hostList(o_hostList),
hostCount(o_hostList.size()),
stateMachine(std::move(o_stateMachine)),
commitIndex(-1),
lastApplied(-1),
currentLeader(-1),
matchIndex(o_hostList.size()),
nextIndex(o_hostList.size()),
serverState(ServerState::Follower),
storage(storageDir + "/" + o_hostList[o_id]) { }

void RaftServer::Run() {
  // Initialize stubs to other servers
  for (int i = 0; i < hostCount; i++) {
    if (i == id) {
      stubs.push_back(nullptr);
    } else {
      stubs.emplace_back(Raft::NewStub(
        grpc::CreateChannel(hostList[i], grpc::InsecureChannelCredentials())));
    }
  }

  /* Set timer callback */
  int old_errno = errno;
  errno = 0;
  signal(SIGALRM, &HandleSignal);
  if (errno) {
    std::perror("Error occurred while trying to set signal handler: ");
    errno = old_errno;
    return;
  }
  errno = old_errno;
  alarmHandlerServer = this;

  storage.LoadFromStorage(&currentTerm, &votedFor, &log);

  ServerBuilder builder;
  builder.AddListeningPort(hostList[id], grpc::InsecureServerCredentials());
  builder.RegisterService(this);
  builder.RegisterService(&clientService);
  cq = builder.AddCompletionQueue();
  server = builder.BuildAndStart();

  srand(time(NULL) + id);
  ResetElectionTimeout();
  SetAlarm(electionTimeout);

  ServeClients();
}

void RaftServer::ServeClients() {
  while (true) {
    ServerContext context;
    responders.emplace_back(&context);
    ServeClientRequest request;
    clientService.RequestServeClient(
      &context, &request, &responders.back(), cq.get(), cq.get(),
      (void*)(&responders.back()));

    void* tag;
    bool ok = false;
    do {
      cq->Next(&tag, &ok);
      // std::cout << "GOT TAG = " << (long long) tag << ", OK = " << ok << "\n";
    } while(tag != (void*)&responders.back());

    if (serverState != ServerState::Leader) {
      ServeClientResponse response;
      response.set_success(false);
      response.set_leaderid(currentLeader);
      responders.back().Finish(response, Status::OK, (void*)&responders.back());
      // std::cout << "RESPONDING TO TAG = " << (long long) tag << '\n';
    } else {
      LogEntry entry;
      entry.set_command(request.command());
      entry.set_term(currentTerm);
      log.push_back(entry);
      matchIndex[id] = log.size() - 1;
      storage.UpdateLog(log.cend() - 1, log.cend(), true);

      ServeClientResponse response;
      response.set_success(true);
      response.set_leaderid(currentLeader);
      responders.back().Finish(response, Status::OK, (void*)&responders.back());
      // std::cout << "APPENDING ENTRY FROM TAG = " << (long long) tag << '\n';
      std::cerr << '\n';
      PrintLog();
      std::cerr << '\n';
    }
  }
}

void RaftServer::Wait() {
  server->Wait();
}

void RaftServer::BecomeFollower() {
  serverState = ServerState::Follower;
}

void RaftServer::BecomeCandidate() {
  serverState = ServerState::Candidate;
  currentTerm++;

  std::cerr << "BecomeCandidate; term=" << currentTerm << '\n';

  votedFor = id;
  storage.UpdateState(currentTerm, votedFor);
  ResetElectionTimeout();
  SetAlarm(electionTimeout);

  RunForElection();
}

void RaftServer::InvokeRequestVote(int voterId, std::atomic<int> *votesGained) {
  ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 
    std::chrono::milliseconds(heartbeatInterval));

  RequestVoteRequest request;
  request.set_term(currentTerm);
  request.set_candidateid(id);
  request.set_lastlogindex(log.size() - 1);
  request.set_lastlogterm(log.empty() ? -1 : log.back().term());

  RequestVoteResponse response;

  std::cerr << "Sending RequestVote to " << voterId << '\n';
  auto status = stubs[voterId]->RequestVote(&context, request, &response);
  
  std::cerr << "Vote request to " << voterId << ": " << status.error_message() << '\n';

  // stateLock.lock();
  // std::cerr << "ENTERED STATELOCK\n";
  if(!status.ok() || currentTerm != request.term() ) {
    return;
  }

  if (response.term() > currentTerm) {
    currentTerm = response.term();
    BecomeFollower();
    return;
  }

  if (response.votegranted()) {
    (*votesGained)++;
  }
  std::cerr << "Gained " << (*votesGained) << " votes.\n";
  // stateLock.unlock();
}

void RaftServer::RunForElection() {
  int initialTerm = currentTerm;

  // Includes self-vote.
  std::atomic<int> votesGained(1);
  for (int i = 0; i < hostCount; i++) {
    if (i != id) {
      std::thread(&RaftServer::InvokeRequestVote, this, i, &votesGained).detach();
    }
  }

  while (votesGained <= hostCount/2 && serverState == ServerState::Candidate &&
         currentTerm == initialTerm) {
  }

  if (votesGained > hostCount/2 && serverState == ServerState::Candidate) {
    BecomeLeader();
  }
}

void RaftServer::InvokeAppendEntries(int o_id) {
  ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 
    std::chrono::milliseconds(RaftServer::heartbeatInterval));

  AppendEntriesRequest request;
  request.set_term(currentTerm);
  request.set_leaderid(id);
  request.set_prevlogindex(nextIndex[o_id] - 1);
  request.set_prevlogterm(
    (nextIndex[o_id] - 1 == -1) ? -1 : log[nextIndex[o_id] - 1].term());

  std::cerr << o_id;
  // std::cerr << "Sending AppendEntries to " << o_id << '\n';
  // std::cerr << "nextIndex = " << nextIndex[o_id] << ", following entries: ";
  for (int i = nextIndex[o_id]; i < log.size(); i++) {
    *request.add_entries() = log[i];
    // std::cerr << log[i].command() << ' ';
  }
  // std::cerr << '\n';

  AppendEntriesResponse response;

  auto status = stubs[o_id]->AppendEntries(&context, request, &response);

  if (!status.ok()) {
    return;
  }
  
  if (response.term() > currentTerm) {
    currentTerm = response.term();
    BecomeFollower();
    return;
  }

  if (response.success()) {
    matchIndex[o_id] = log.size() - 1;
    nextIndex[o_id] = log.size();

    while(true) {
      int count = 0;
      for (int i = 0; i < hostCount; i++) {
        if (matchIndex[i] > commitIndex) {
          count++;
        }
      }
      if (count > hostCount/2) {
        commitIndex++;
        while (lastApplied < commitIndex) {
          lastApplied++;
          stateMachine->Apply(log[lastApplied].command());
        }
      } else {
        break;
      }
    }
  } else {
    nextIndex[o_id]--;
  }
}

void RaftServer::ReplicateEntries() {
  SetAlarm(heartbeatInterval);

  for (int i = 0; i < hostCount; i++) {
    if (i != id) {
      std::thread(&RaftServer::InvokeAppendEntries, this, i).detach();
    }
  }
}

void RaftServer::BecomeLeader() {
  std::cerr << "Becoming the leader!\n";
  currentLeader = id;
  serverState = ServerState::Leader;
  firstResponderIndex = log.size();
  for (int i = 0; i < hostCount; i++) {
    nextIndex[i] = log.size();
    matchIndex[i] = -1;
  }
  SetAlarm(heartbeatInterval);
}

void RaftServer::AlarmCallback() {
  if (serverState == ServerState::Leader) {
    ReplicateEntries();
  } else {
    BecomeCandidate();
  }
}

void RaftServer::PrintLog() {
  std::cerr << "log = [";
  for (auto entry : log) {
    std::cerr << '(' << entry.term() << ", " << entry.command() << "), ";
  }
  std::cerr << "]\n";
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
    currentTerm = request->term();
    BecomeFollower();
  }

  SetAlarm(electionTimeout);
  currentLeader = request->leaderid();
  if (request->prevlogindex() >= int(log.size()) ||
      (request->prevlogindex() != -1 && 
       log[request->prevlogindex()].term() != request->prevlogterm())) {
    return Status::OK;
  }

  uint curLogIndex = request->prevlogindex() + 1;
  bool mustRecreateLog = false;
  int startIndex = curLogIndex;
  for (LogEntry entry: request->entries()) {
    if (curLogIndex < log.size()) {
      if (log[curLogIndex].term() == entry.term()) {
        startIndex++;
      } else {
        log.resize(curLogIndex);
        mustRecreateLog = true;
        startIndex = 0;
      }
    }
    if (curLogIndex >= log.size()) {
      log.push_back(entry);
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

  if (request->entries_size() > 0) {
    std::cerr << '\n';
    PrintLog();
    std::cerr << '\n';
  }

  return Status::OK;
}

Status RaftServer::RequestVote(ServerContext* context,
                             const RequestVoteRequest* request,
                             RequestVoteResponse* response) {
  std::cerr << "Got RequestVote from " << request->candidateid() << ".\n";
  std::lock_guard<std::mutex> lock(overallLock);

  response->set_term(currentTerm);
  response->set_votegranted(false);

  if(request->term() < currentTerm) {
    return Status::OK;
  }

  if (request->term() > currentTerm) {
    currentTerm = request->term();
    BecomeFollower();
  } else if(votedFor != -1 && votedFor != request->candidateid()) {
    return Status::OK;
  }

  if (!log.empty() && (log.back().term() > request->lastlogterm() ||
     (log.back().term() == request->lastlogterm() &&
      log.size() - 1 > request->lastlogindex()))) {
    return Status::OK;
  }

  SetAlarm(electionTimeout);
  storage.UpdateState(currentTerm, votedFor);
  votedFor = request->candidateid();
  response->set_votegranted(true);

  std::cerr << "Granting vote to " << request->candidateid() << ".\n";

  return Status::OK;
}

void RaftServer::SetAlarm(int after_ms) {
  if (serverState == ServerState::Follower) {
    std::cerr << ".";
  }

  struct itimerval timer;

  /* Configure timer intervals */
  timer.it_value.tv_sec = after_ms / 1000;
  timer.it_value.tv_usec = 1000 * (after_ms % 1000); // microseconds

  timer.it_interval = timer.it_value;

  /* Set timer */
  int old_errno = errno;
  errno = 0;
  setitimer(ITIMER_REAL, &timer, nullptr);
  if(errno) {
    std::perror("An error occurred while trying to set the timer");
  }
  errno = old_errno;
  return;
}

void RaftServer::ResetElectionTimeout() {
  electionTimeout = RaftServer::minElectionTimeout + (rand() % 
    (RaftServer::maxElectionTimeout - RaftServer::minElectionTimeout + 1));
}
