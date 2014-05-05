#include "config.h"

#include "acceptsocket.h"
#include "clientsocket.h"
#include "util.h"
#include "configmap.h"
#include "enigma_settings.h"

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <syslog.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/inotify.h>

#include <sstream>
using std::ostringstream;

#include <fstream>
using std::ifstream;

#include <string>
using std::string;

#include <vector>
using std::vector;

#include <map>
using std::map;

#include <boost/program_options.hpp>
namespace bpo = boost::program_options;

typedef struct
{
	ClientSocket::default_streaming_action	default_action;
	AcceptSocket							*accept_socket;
} listen_socket_t;

typedef map<string, listen_socket_t> listen_action_t;

static char	*const *global_argv;
static char	*const *global_arge;

static void reexec(void)
{
	char path[256];
	ssize_t length;

	if((length = readlink("/proc/self/exe", path, sizeof(path))) <= 0)
	{
		Util::vlog("streamproxy: re-exec failed (readlink), quitting");
		exit(1);
	}

	path[length] = '\0';

	if(execve(path, global_argv, global_arge))
	{
		Util::vlog("streamproxy: re-exec failed (execve), quitting");
		exit(1);
	}
}

static void sigchld(int) // prevent Z)ombie processes
{
	siginfo_t infop;

	infop.si_pid = 0;

	waitid(P_ALL, 0, &infop, WEXITED | WNOHANG);

	Util::vlog("streamproxy: pid %d exited", infop.si_pid);

	if(infop.si_pid)
	{
		if((infop.si_code == CLD_KILLED) || (infop.si_code == CLD_DUMPED))
		{
			if(infop.si_status == SIGSEGV)
				Util::vlog("streamproxy: child process %d exited with segmentation fault", infop.si_pid);
			else
				Util::vlog("streamproxy: child process %d was killed", infop.si_pid);
		}
	}
	else
		Util::vlog("streamproxy: sigchld called but no childeren to wait for");
}

int main(int argc, char *const argv[], char *const arge[])
{
	bpo::options_description	options("Use single or multiple pairs of port_number:default_action either with --listen or positional");
	ostringstream				convert;
	int							inotify_fd = -1;

	global_argv = argv;
	global_arge = arge;

	try
	{
		struct sigaction						signal_action;
		StringVector							listen_parameters;
		string									require_auth_group;
		StringVector::const_iterator			it;
		listen_action_t							listen_action;
		listen_action_t::iterator				it2;
		size_t									ix;
		ssize_t									rv;
		string									port;
		string									action_str;
		ClientSocket::default_streaming_action	action;
		struct pollfd							*pfd;
		size_t									pfds;
		int										new_socket;
		static const char						*action_name[2] = { "stream", "transcode" };
		EnigmaSettings							settings;
		bpo::positional_options_description		positional_options;
		bpo::variables_map						vm;
		ifstream								config_file("/etc/enigma2/streamproxy.conf");
		string									option_default_size;
		string									option_default_bitrate;
		string									option_default_profile;
		string									option_default_level;
		string									option_default_bframes;
		ConfigMap								config_map;

		positional_options.add("listen", -1);

		options.add_options()
			("foreground,f",	bpo::bool_switch(&Util::foreground)->implicit_value(true),		"run in foreground (don't become a daemon)")
			("group,g",			bpo::value<string>(&require_auth_group),						"require streaming users to be member of this group")
			("listen,l",		bpo::value<StringVector>(&listen_parameters),					"listen to tcp port with default action")
			("size,s",			bpo::value<string>(&option_default_size),						"default transcoding frame size (480p (default), 576p or 720p)")
			("bitrate,b",		bpo::value<string>(&option_default_bitrate),					"default transcoding bit rate (100 - 10000 kbps)(default 500)")
			("profile,P",		bpo::value<string>(&option_default_profile),					"default transcoding h264 profile (baseline (default), main, high)")
			("level,L",			bpo::value<string>(&option_default_level),						"default transcoding h264 level (3.1 (default), 3.2, 4.0)")
			("bframes,B",		bpo::value<string>(&option_default_bframes),					"default transcoding h264 b frames (0 (default), 1 or 2)");

		if(config_file)
		{
			bpo::store(bpo::parse_config_file(config_file, options), vm);
			bpo::notify(vm);
		}

		bpo::store(bpo::command_line_parser(argc, argv).options(options).positional(positional_options).run(), vm);
		bpo::notify(vm);

		config_file.close();

		config_map["foreground"]	= ConfigValue(Util::foreground);
		config_map["group"]			= ConfigValue(require_auth_group);
		config_map["size"]			= ConfigValue(option_default_size);
		config_map["bitrate"]		= ConfigValue(option_default_bitrate);
		config_map["profile"]		= ConfigValue(option_default_profile);
		config_map["level"]			= ConfigValue(option_default_level);
		config_map["bframes"]		= ConfigValue(option_default_bframes);

		if(settings.exists("config.OpenWebif.auth") && settings.as_string("config.OpenWebif.auth_for_streaming") == "true")
			config_map["auth"] = ConfigValue(true);
		else
			config_map["auth"] = ConfigValue(false);

		if(settings.exists("config.OpenWebif.port"))
			config_map["webifport"] = ConfigValue(settings.as_string("config.OpenWebif.port"));
		else
			config_map["webifport"] = ConfigValue(80);

		for(it = listen_parameters.begin(); it != listen_parameters.end(); it++)
		{
			ix = it->find(':');

			if((ix == string::npos) || (ix == 0))
				throw(string("positional parameter should consist of <port>:<default action>"));

			port		= it->substr(0, ix);
			action_str	= it->substr(ix + 1);

			if(action_str == "stream")
				action = ClientSocket::action_stream;
			else
				if(action_str == "transcode")
					action = ClientSocket::action_transcode;
				else
					throw(string("default action should be either stream or transcode"));

			listen_action[port].default_action = action;

			config_map[string("listen:") + port] = action;
		}

		ConfigMap::const_iterator it3;

		for(it3 = config_map.begin(); it3 != config_map.end(); it3++)
			Util::vlog("streamproxy: config_map: %s = %s/%d", it3->first.c_str(), it3->second.string_value.c_str(), it3->second.int_value);

		if((pfds = listen_action.size() + 1) < 2)
			throw(string("no listen_port:default_action parameters given"));

		if(!Util::foreground && daemon(0, 0))
			throw(string("daemon() gives error"));

		signal_action.sa_handler = sigchld;
		signal_action.sa_flags = SA_NOCLDSTOP | SA_NODEFER | SA_RESTART;
		signal_action.sa_restorer = 0;

		sigemptyset(&signal_action.sa_mask);

		sigaction(SIGCHLD, &signal_action, 0);

		if((inotify_fd = inotify_init1(IN_CLOEXEC)) < 0)
			throw(string("inotify_init error"));

		if(inotify_add_watch(inotify_fd, "/etc/enigma2/streamproxy.conf", IN_MODIFY | IN_CREATE | IN_MOVE_SELF | IN_ATTRIB) < 0)
			throw(string("inotify_add_watch error (streamproxy config file)"));

		if(inotify_add_watch(inotify_fd, "/etc/enigma2/settings", IN_MODIFY | IN_CREATE | IN_MOVE_SELF | IN_ATTRIB) < 0)
			throw(string("inotify_add_watch error (enigma config)"));

		pfd = new struct pollfd[pfds];

		pfd[0].fd		= inotify_fd;
		pfd[0].events	= POLLIN;

		for(it2 = listen_action.begin(), ix = 1; it2 != listen_action.end(); it2++, ix++)
		{
			it2->second.accept_socket = new AcceptSocket(it2->first);
			pfd[ix].fd		= it2->second.accept_socket->get_fd();
			pfd[ix].events	= POLLIN;
			//Util::vlog("> %s -> %s,%d\n", it2->first.c_str(), action_name[it2->second.default_action], it2->second.accept_socket->get_fd());
		}

		for(;;)
		{
			errno = 0;

			if((rv = poll(pfd, pfds, -1)) < 0)
			{
				if(errno == EINTR)
					errno = 0;
				else
					throw(string("poll error"));
			}

			if(pfd[0].revents & POLLIN)
			{
				Util::vlog("streamproxy: config file change detected, restarting");
				reexec();
			}

			for(ix = 1; ix < pfds; ix++)
			{
				if(pfd[ix].revents & (POLLERR | POLLNVAL | POLLHUP))
					throw(string("poll error on fd"));

				if(!(pfd[ix].revents & POLLIN))
					continue;

				for(it2 = listen_action.begin(); it2 != listen_action.end(); it2++)
					if(it2->second.accept_socket->get_fd() == pfd[ix].fd)
						break;

				if(it2 == listen_action.end())
					throw(string("poll success on non-existent fd"));

				new_socket = it2->second.accept_socket->accept();

				Util::vlog("streamproxy: accept new connection on port %s, default action: %s, fd %d",
						it2->first.c_str(), action_name[it2->second.default_action], new_socket);

				if(fork()) // parent
					close(new_socket);
				else
				{
					(void)ClientSocket(new_socket, it2->second.default_action, config_map);
					_exit(0);
				}

				usleep(100000); // runaway protection
			}
		}
	}
	catch(const string &e)
	{
		fprintf(stderr, "streamproxy: %s\n", e.c_str());
		exit(1);
	}
	catch(bpo::error &e)
	{
		fprintf(stderr, "streamproxy: %s\n", e.what());
		convert.str("");
		convert << options;
		fprintf(stderr, "streamproxy: %s\n", convert.str().c_str());
		exit(1);
	}
	catch(...)
	{
		Util::vlog("streamproxy: default exception");
		exit(1);
	}

	if(inotify_fd >= 0)
		close(inotify_fd);

	return(0);
}
