#include "config.h"
#include "trap.h"

#include "time_offset.h"

#include <string>
using::std::string;

#include <boost/range/algorithm/count.hpp>

#include <stdlib.h>

TimeOffset::TimeOffset(string timestring)
{
	int c;
	size_t ix;
	string part;

	if(timestring.length() < 1)
		throw(trap("TimeOffset: invalid timestring, empty"));

	c = boost::count(timestring, ':');
	seconds = 0;

	if(c > 2)
	{
		throw(trap("TimeOffset: invalid timestring, to many colons"));
	}

	if(c > 1)
	{
		ix = timestring.find(':');
		part = timestring.substr(0, ix);
		seconds += strtoul(part.c_str(), 0, 10) * 60 * 60;
		timestring.erase(0, ix + 1);
	}

	if(c > 0)
	{
		ix = timestring.find(':');
		part = timestring.substr(0, ix);
		seconds += strtoul(part.c_str(), 0, 10) * 60;
		timestring.erase(0, ix + 1);
	}

	if(c == 0)
	{
		ix = timestring.find(':');
		part = timestring.substr(0, ix);
		seconds += strtoul(part.c_str(), 0, 10);
	}
}

int TimeOffset::as_seconds() const
{
	return(seconds);
}
