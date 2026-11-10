/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "PreRTS.h"
#include "GameClient/ClientInstance.h"

// GeneralsX: GeneralsX owns its own single-instance mutex name.
//
// 685EAFF2-3216-4265-B047-251C5F4B82F3 is retail Generals' GUID, and the Generals Online client
// uses the identical value (references/generals-online-client/.../ClientInstance.cpp:21). Sharing
// it means retail, Generals Online and GeneralsX all contend for one mutex, so a GeneralsX build
// launched while Generals Online is running gets ERROR_ALREADY_EXISTS, initialize() returns FALSE
// and the process refuses to start. On the Windows test box that is the normal state, not an edge
// case - the operator plays Generals Online there.
//
// Do NOT "fix" that by turning on multi-instance for the headless LAN roles. isMultiInstance()
// also gates IPEnumeration::getAddresses injecting a synthetic 127.<instance> address
// (IPEnumeration.cpp:118-127); ipSortClass only deprioritises 169.254, so 127.x sorts ahead of the
// real NIC and the peer advertises itself on loopback where no other machine can reach it. See the
// note above setUpHeadlessLanRole in CommandLine.cpp. Instance naming and address synthesis are
// separate concerns and only the first one is wanted here.
//
// A private GUID keeps the guard doing its real job - one GeneralsX at a time - while making it
// structurally impossible to collide with a product we do not control. This file is not in the
// SimID digest path list (simsourcedigest_watcher.cmake digests Core/GameEngine/{Include,Source}/
// {Common,GameLogic,GameNetwork} only), so changing it does not move sourceID and does not force a
// lockstep rebuild of every peer.
#define GENERALS_GUID "BA1205E2-B5D5-47B2-BF0A-FEF0B970EAA9"

namespace rts
{
HANDLE ClientInstance::s_mutexHandle = nullptr;
UnsignedInt ClientInstance::s_instanceIndex = 0;

#if defined(RTS_MULTI_INSTANCE)
Bool ClientInstance::s_isMultiInstance = true;
#else
Bool ClientInstance::s_isMultiInstance = false;
#endif

bool ClientInstance::initialize()
{
	if (isInitialized())
	{
		return true;
	}

	// Create a mutex with a unique name to Generals in order to determine if our app is already running.
	// WARNING: DO NOT use this number for any other application except Generals.
	while (true)
	{
		if (isMultiInstance())
		{
			std::string guidStr = getFirstInstanceName();
			if (s_instanceIndex > 0u)
			{
				char idStr[33];
				itoa(s_instanceIndex, idStr, 10);
				guidStr.push_back('-');
				guidStr.append(idStr);
			}
			s_mutexHandle = CreateMutex(nullptr, FALSE, guidStr.c_str());
			if (GetLastError() == ERROR_ALREADY_EXISTS)
			{
				if (s_mutexHandle != nullptr)
				{
					CloseHandle(s_mutexHandle);
					s_mutexHandle = nullptr;
				}
				// Try again with a new instance.
				++s_instanceIndex;
				continue;
			}
		}
		else
		{
			s_mutexHandle = CreateMutex(nullptr, FALSE, getFirstInstanceName());
			if (GetLastError() == ERROR_ALREADY_EXISTS)
			{
				if (s_mutexHandle != nullptr)
				{
					CloseHandle(s_mutexHandle);
					s_mutexHandle = nullptr;
				}
				return false;
			}
		}
		break;
	}

	return true;
}

bool ClientInstance::isInitialized()
{
	return s_mutexHandle != nullptr;
}

bool ClientInstance::isMultiInstance()
{
	return s_isMultiInstance;
}

void ClientInstance::setMultiInstance(bool v)
{
	if (isInitialized())
	{
		DEBUG_CRASH(("ClientInstance::setMultiInstance(%d) - cannot set multi instance after initialization", (int)v));
		return;
	}
	s_isMultiInstance = v;
}

void ClientInstance::skipPrimaryInstance()
{
	if (isInitialized())
	{
		DEBUG_CRASH(("ClientInstance::skipPrimaryInstance() - cannot skip primary instance after initialization"));
		return;
	}
	s_instanceIndex = 1;
}

UnsignedInt ClientInstance::getInstanceIndex()
{
	DEBUG_ASSERTLOG(isInitialized(), ("ClientInstance::isInitialized() failed"));
	return s_instanceIndex;
}

UnsignedInt ClientInstance::getInstanceId()
{
	return getInstanceIndex() + 1;
}

const char* ClientInstance::getFirstInstanceName()
{
	return GENERALS_GUID;
}

} // namespace rts
