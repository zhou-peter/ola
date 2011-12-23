/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * dmx-trigger.cpp
 * Copyright (C) 2011 Simon Newton
 */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <sysexits.h>

#include <ola/Callback.h>
#include <ola/DmxBuffer.h>
#include <ola/Logging.h>
#include <ola/OlaCallbackClient.h>
#include <ola/OlaClientWrapper.h>
#include <ola/network/SelectServer.h>

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "tools/dmx_trigger/Action.h"
#include "tools/dmx_trigger/Context.h"
#include "tools/dmx_trigger/DMXTrigger.h"
#include "tools/dmx_trigger/ParserGlobals.h"

using ola::DmxBuffer;
using std::map;
using std::vector;

// prototype of bison-generated parser function
int yyparse();

// globals modified by the config parser
Context *global_context;
SlotActionMap global_slot_actions;

// The SelectServer to kill when we catch SIGINT
ola::network::SelectServer *ss = NULL;

// The options struct
typedef struct {
  bool help;
  ola::log_level log_level;
  unsigned int universe;
  vector<string> args;  // extra args
} options;


/*
 * Parse our command line options
 */
void ParseOptions(int argc, char *argv[], options *opts) {
  static struct option long_options[] = {
      {"help", no_argument, 0, 'h'},
      {"log-level", required_argument, 0, 'l'},
      {"universe", required_argument, 0, 'u'},
      {0, 0, 0, 0}
    };

  int option_index = 0;

  while (1) {
    int c = getopt_long(argc, argv, "hl:u:", long_options, &option_index);

    if (c == -1)
      break;

    switch (c) {
      case 'h':
        opts->help = true;
        break;
      case 'l':
        switch (atoi(optarg)) {
          case 0:
            // nothing is written at this level
            // so this turns logging off
            opts->log_level = ola::OLA_LOG_NONE;
            break;
          case 1:
            opts->log_level = ola::OLA_LOG_FATAL;
            break;
          case 2:
            opts->log_level = ola::OLA_LOG_WARN;
            break;
          case 3:
            opts->log_level = ola::OLA_LOG_INFO;
            break;
          case 4:
            opts->log_level = ola::OLA_LOG_DEBUG;
            break;
          default :
            break;
        }
        break;
      case 'u':
        opts->universe = atoi(optarg);
        break;
      case '?':
        break;
      default:
       break;
    }
  }

  int index = optind;
  for (; index < argc; index++)
    opts->args.push_back(argv[index]);
  return;
}


/*
 * Display the help message
 */
void DisplayHelpAndExit(char *argv[]) {
  std::cout << "Usage: " << argv[0] << " [options] <config_file>\n"
  "\n"
  "Run programs based on the values in a DMX stream.\n"
  "\n"
  "  -h, --help                Display this help message and exit.\n"
  "  -l, --log-level <level>   Set the loggging level 0 .. 4.\n"
  "  -u, --universe <universe> The universe to use.\n"
  << std::endl;
  exit(0);
}


/*
 * Catch SIGCHLD.
 */
static void CatchSIGCHLD(int signo) {
  pid_t pid;
  do {
    pid = waitpid(-1, NULL, WNOHANG);
  } while (pid > 0);
  (void) signo;
}


/*
 * Terminate cleanly on interrupt
 */
static void CatchSIGINT(int signo) {
  // there is a race condition here if you send the signal before we call Run()
  // it's not a huge deal though.
  if (ss)
    ss->Terminate();
  (void) signo;
}


/*
 * Install the SIGCHLD handler.
 */
bool InstallSignals() {
  struct sigaction act, oact;

  act.sa_handler = CatchSIGCHLD;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;

  if (sigaction(SIGCHLD, &act, &oact) < 0) {
    OLA_WARN << "Failed to install signal SIGCHLD";
    return false;
  }

  act.sa_handler = CatchSIGINT;
  if (sigaction(SIGINT, &act, &oact) < 0) {
    OLA_WARN << "Failed to install signal SIGINT";
    return false;
  }
  if (sigaction(SIGTERM, &act, &oact) < 0) {
    OLA_WARN << "Failed to install signal SIGTERM";
    return false;
  }
  return true;
}


/**
 * The DMX Handler, this calls the trigger if the universes match.
 */
void NewDmx(unsigned int our_universe,
            DMXTrigger *trigger,
            unsigned int universe,
            const DmxBuffer &data,
            const std::string &error) {
  if (universe == our_universe) {
    if (error.empty())
      trigger->NewDMX(data);
  }
  (void) error;
}


/*
 * Main
 */
int main(int argc, char *argv[]) {
  options opts;
  opts.log_level = ola::OLA_LOG_INFO;
  opts.universe = 1;
  opts.help = false;
  ParseOptions(argc, argv, &opts);

  if (opts.help)
    DisplayHelpAndExit(argv);

  if (opts.args.size() != 1)
    DisplayHelpAndExit(argv);

  ola::InitLogging(opts.log_level, ola::OLA_LOG_STDERR);

  // setup the defaut context
  global_context = new Context();
  OLA_INFO << "Loading config from " << opts.args[0];

  // open the config file
  if ((argc > 1) && (freopen(argv[1], "r", stdin) == NULL)) {
    OLA_FATAL << argv[0] << ": File " << argv[1] << " cannot be opened.\n";
    exit(EX_DATAERR);
  }

  yyparse();

  // if we got to this stage the config is ok, setup the client
  ola::OlaCallbackClientWrapper wrapper;

  if (!wrapper.Setup())
    exit(EX_UNAVAILABLE);

  ss = wrapper.GetSelectServer();

  if (!InstallSignals())
    exit(EX_OSERR);

  // create the vector of SlotActions
  vector<SlotActions*> slot_actions;
  slot_actions.reserve(global_slot_actions.size());
  SlotActionMap::const_iterator iter = global_slot_actions.begin();
  for (; iter != global_slot_actions.end(); ++iter)
    slot_actions.push_back(iter->second);

  // setup the context and trigger
  Context context;
  DMXTrigger trigger(&context, slot_actions);

  // register for DMX
  ola::OlaCallbackClient *client = wrapper.GetClient();
  client->SetDmxCallback(
      ola::NewCallback(&NewDmx, opts.universe, &trigger));
  client->RegisterUniverse(opts.universe, ola::REGISTER, NULL);

  // start the client
  wrapper.GetSelectServer()->Run();
}
