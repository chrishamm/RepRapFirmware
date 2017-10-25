/*
 * IP4String.cpp
 *
 *  Created on: 19 Sep 2017
 *      Author: David
 */

#include "IP4String.h"
#include <stdio.h>		// FIXME this should be changed to cstdio when upgrading to a new compiler version

IP4String::IP4String(const uint8_t ip[4])
{
	snprintf(buf, sizeof(buf)/sizeof(buf[0]), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

IP4String::IP4String(uint32_t ip)
{
	snprintf(buf, sizeof(buf)/sizeof(buf[0]), "%u.%u.%u.%u",
				(unsigned int)(ip & 0xFFu), (unsigned int)((ip >> 8) & 0xFFu), (unsigned int)((ip >> 16) & 0xFFu), (unsigned int)((ip >> 24) & 0xFFu));
}

// End
