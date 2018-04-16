#include "Storage.h"
#include <cstdlib>

using raft::LogEntry;

void output(const Storage& s) {
  int currentTerm, votedFor;
  std::vector<LogEntry> entries;
  s.LoadFromStorage(&currentTerm, &votedFor, &entries);
  std::cout << currentTerm << ' ' << votedFor << ' ' << entries.size() << '\n';
  for (auto log : entries) {
    std::cout << '(' << log.term() << ", \"" << log.command() << "\"), ";
  }
  std::cout << '\n';
}

int main() {
  Storage s(".");
  s.UpdateState(5, 100);
  output(s);
  s.UpdateState(11, 7);
  output(s);
  std::vector<LogEntry> entries;
  LogEntry l;
  l.set_term(3);
  l.set_command("this is a command");
  entries.push_back(l);
  l.set_term(9);
  l.set_command("request #2");
  entries.push_back(l);
  s.UpdateLog(entries.cbegin(), entries.cend(), true);
  output(s);
  s.UpdateLog(entries.cbegin(), entries.cend(), false);
  output(s);
}
