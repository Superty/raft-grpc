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

  // while (true) {
  //   ClientContext context;
  //   RequestVoteResponse response;
  //   RequestVoteRequest request;
  //   request.set_term(currentTerm);
  //   request.set_candidateid(id);
  //   request.set_lastlogindex(log.size() - 1);
  //   std::cerr << "Sending request to server" <<  (id ^ 1) << "\n";
  //   auto status = stubs[id ^ 1]->RequestVote(&context, request, &response);
  //   std::cout << status.error_message() << '\n' << response.term() << '\n';
  // }

  srand(time(NULL));
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

    ServeClientResponse response;
    void* tag;
    bool ok = false;
    do {
      cq->Next(&tag, &ok);
    } while(tag != (void*)&responders.back());

    if (serverState != ServerState::Leader) {
      response.set_success(true);
      responders.begin()->Finish(response, Status::OK, (void*)&responders.back());
    } else {
      LogEntry entry;
      entry.set_command(request.command());
      entry.set_term(currentTerm);
      log.push_back(entry);
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
  std::cerr << "BecomeCandidate; term=" << currentTerm << '\n';
  // std::lock_guard<std::mutex> lock(overallLock);
  // Narrate("Becoming candidate. Current Term = ");

  serverState = ServerState::Candidate;
  currentTerm++;
  votedFor = id;
  storage.UpdateState(currentTerm, votedFor);
  ResetElectionTimeout();
  SetAlarm(electionTimeout);

  RunForElection();
}

void RaftServer::RunForElection() {
  std::vector<std::unique_ptr<
    ClientAsyncResponseReader<RequestVoteResponse>>> rpcs(hostCount);
  std::vector<Status> statuses(hostCount);
  std::vector<RequestVoteResponse> responses(hostCount);
  std::vector<ClientContext> contexts(hostCount);

  CompletionQueue clientCq;

  RequestVoteRequest request;
  request.set_term(currentTerm);
  request.set_candidateid(id);
  request.set_lastlogindex(log.size() - 1);
  if (!log.empty()) {
    request.set_lastlogterm(log.back().term());
  }

  for(int i = 0; i < hostCount; i++) {
    if(i != id) {
      rpcs[i] = std::move(stubs[i]->AsyncRequestVote(&contexts[i], request, &clientCq));
      rpcs[i]->Finish(&responses[i], &statuses[i], (void*)int64_t(i));
    }
  }

  std::cerr << "Called AsyncNext in term " << request.term() << '\n';

  // Includes self-vote.
  int votesGained = 1;
  while (votesGained <= hostCount/2) {
    void* tag;
    bool ok = false;
    auto nextStatus = clientCq.AsyncNext(&tag, &ok,
      std::chrono::system_clock::now() + 
      std::chrono::milliseconds(electionTimeout + maxElectionTimeout));
    int i = (long long)tag;

    // std::cerr << "Out of AsyncNext in term " << request.term()
    //           << "; CurrentTerm = " << currentTerm << "; Status = "
              // << nextStatus << '\n';

    if (nextStatus == CompletionQueue::NextStatus::TIMEOUT || 
        currentTerm != request.term() || serverState != ServerState::Candidate) {
      clientCq.Shutdown();
      std::cerr << "timeout in term " << request.term() << '\n';
      // std::cerr << "Blocking on next in term " << request.term() <<'\n';
      // while (clientCq.Next(&tag, &ok));
      // std::cerr << "Next returns " << clientCq.Next(&tag, &ok) << '\n';
      return;
    }

    std::cerr << "currentTerm =" << currentTerm << ", response.term = " << responses[i].term()
              << ", voted=" << responses[i].votegranted() << '\n';

    if (responses[i].term() > currentTerm) {
      currentTerm = responses[i].term();
      BecomeFollower();
    }

    if (responses[i].votegranted()) {
      votesGained++;
    }

  }

  BecomeLeader();
  clientCq.Shutdown();
  void *tag;
  bool ok = false;
  // while (clientCq.Next(&tag, &ok));
}

void RaftServer::ReplicateEntries() {
  Narrate("We are the leader; replicating entries.");
}

void RaftServer::BecomeLeader() {
  Narrate("Becoming the leader!");
  serverState = ServerState::Leader;
  firstResponderIndex = log.size();
}

void RaftServer::AlarmCallback() {
  // Narrate("Alarm timed out!");
  if (serverState == ServerState::Leader) {
    ReplicateEntries();
  } else {
    BecomeCandidate();
  }
}

void RaftServer::Narrate(const string& text) {
  std::cout << text << '\n';
}

// void RaftServer::RequestVoteCallback(int responseTerm, bool voteGranted) {
//   std::lock_guard<std::mutex> lock(overallLock);
//   if (responseTerm > currentTerm) {
//     currentTerm = responseTerm;
//     BecomeFollower();
//     return;
//   }

//   if (voteGranted) {
//     votesGained++;
//   }
//   if (votesGained > hostCount/2) {
//     // BecomeLeader();
//   }
// }

Status RaftServer::AppendEntries(ServerContext* context,
                                   const AppendEntriesRequest* request,
                                   AppendEntriesResponse *response) {
  // std::lock_guard<std::mutex> lock(overallLock);
  SetAlarm(electionTimeout);

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
  std::cerr << "GOT REQUESTVOTE\n";
  // std::lock_guard<std::mutex> lock(overallLock);

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
  } else if (log.back().term() > request->lastlogterm() ||
     (log.back().term() == request->lastlogterm() &&
      log.size() - 1 > request->lastlogindex())) {
    return Status::OK;
  }

  SetAlarm(electionTimeout);
  storage.UpdateState(currentTerm, votedFor);
  votedFor = request->candidateid();
  response->set_votegranted(true);

  return Status::OK;
}

void RaftServer::SetAlarm(int after_ms) {
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
  // std::cout << "Election Timeout=" << electionTimeout << '\n';
}
