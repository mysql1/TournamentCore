/*
 * Copyright (C) 2014-2015 TournamentCore
 * Copyright (C) 2008-2015 TrinityCore <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TOURNAMENT_PACKETLOG_H
#define TOURNAMENT_PACKETLOG_H

#include "Common.h"

#include <boost/asio/ip/address.hpp>
#include <mutex>

enum Direction
{
    CLIENT_TO_SERVER,
    SERVER_TO_CLIENT
};

class WorldPacket;

class PacketLog
{
    private:
        PacketLog();
        ~PacketLog();
        std::mutex _logPacketLock;
        std::once_flag _initializeFlag;

    public:
        static PacketLog* instance()
        {
            static PacketLog instance;
            return &instance;
        }

        void Initialize();
        bool CanLogPacket() const { return (_file != NULL); }
        void LogPacket(WorldPacket const& packet, Direction direction, boost::asio::ip::address const& addr, uint16 port);

    private:
        FILE* _file;
};

#define sPacketLog PacketLog::instance()
#endif