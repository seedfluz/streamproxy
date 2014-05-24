#include "config.h"
#include "trap.h"

#include "filestreaming.h"
#include "util.h"
#include "queue.h"
#include "mpegts.h"

#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

#include <string>
using std::string;

FileStreaming::FileStreaming(string file, int socket_fd, int pct_offset, int time_offset_s) throw(trap)
{
	size_t			max_fill_socket = 0;
	struct pollfd	pfd;
	string			http_reply = "HTTP/1.1 200 OK\r\n"
						"Content-type: video/mpeg\r\n"
						"Connection: Close\r\n"
						"Server: Streamproxy\r\n"
						"Accept-Ranges: bytes\r\n";
	
	Util::vlog("FileStreaming: streaming file: %s from %d", file.c_str(), time_offset_s);

	MpegTS stream(file, time_offset_s > 0);

	if(pct_offset > 0)
		stream.seek_pct(pct_offset);
	else
		if(stream.is_time_seekable && (time_offset_s > 0))
			stream.seek_time((time_offset_s * 1000) + stream.first_pcr_ms);

	Queue socket_queue(32 * 1024);

	http_reply += "Content-Length: " + Util::uint_to_string(stream.stream_length) + "\r\n";
	http_reply += "\r\n";

	socket_queue.append(http_reply.length(), http_reply.c_str());

	for(;;)
	{
		if(socket_queue.usage() < 50)
		{
			if(!socket_queue.read(stream.get_fd()))
			{
				Util::vlog("FileStreaming: eof");
				break;
			}
		}

		if(socket_queue.usage() > max_fill_socket)
			max_fill_socket = socket_queue.usage();

		pfd.fd		= socket_fd;
		pfd.events	= POLLRDHUP;

		if(socket_queue.length() > 0)
			pfd.events |= POLLOUT;

		if(poll(&pfd, 1, -1) <= 0)
			throw(trap("FileStreaming: poll error"));

		if(pfd.revents & (POLLRDHUP | POLLHUP))
		{
			Util::vlog("FileStreaming: client hung up");
			break;
		}

		if(pfd.revents & (POLLERR | POLLNVAL))
		{
			Util::vlog("FileStreaming: socket error");
			break;
		}

		if(pfd.revents & POLLOUT)
		{
			if(!socket_queue.write(socket_fd))
			{
				Util::vlog("FileStreaming: write socket error");
				break;
			}
		}
	}

	Util::vlog("FileStreaming: streaming ends, socket max queue fill: %d%%", max_fill_socket);
}
