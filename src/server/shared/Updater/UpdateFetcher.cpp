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

#include "UpdateFetcher.h"
#include "Log.h"
#include "Util.h"

#include <fstream>
#include <chrono>
#include <vector>
#include <sstream>
#include <exception>
#include <unordered_map>
#include <openssl/sha.h>

using namespace boost::filesystem;

UpdateFetcher::UpdateFetcher(Path const& sourceDirectory,
    std::function<void(std::string const&)> const& apply,
    std::function<void(Path const& path)> const& applyFile,
    std::function<QueryResult(std::string const&)> const& retrieve) :
        _sourceDirectory(sourceDirectory), _apply(apply), _applyFile(applyFile),
        _retrieve(retrieve)
{
}

UpdateFetcher::LocaleFileStorage UpdateFetcher::GetFileList() const
{
    LocaleFileStorage files;
    DirectoryStorage directories = ReceiveIncludedDirectories();
    for (auto const& entry : directories)
        FillFileListRecursively(entry.path, files, entry.state, 1);

    return files;
}

void UpdateFetcher::FillFileListRecursively(Path const& path, LocaleFileStorage& storage, State const state, uint32 const depth) const
{
    static uint32 const MAX_DEPTH = 10;
    static directory_iterator const end;

    for (directory_iterator itr(path); itr != end; ++itr)
    {
        if (is_directory(itr->path()))
        {
            if (depth < MAX_DEPTH)
                FillFileListRecursively(itr->path(), storage, state, depth + 1);
        }
        else if (itr->path().extension() == ".sql")
        {
            TC_LOG_TRACE("sql.updates", "Added locale file \"%s\".", itr->path().filename().generic_string().c_str());

            LocaleFileEntry const entry = { itr->path(), state };

            // Check for doubled filenames
            // Since elements are only compared through their filenames this is ok
            if (storage.find(entry) != storage.end())
            {
                TC_LOG_FATAL("sql.updates", "Duplicated filename occurred \"%s\", since updates are ordered " \
                    "through its filename every name needs to be unique!", itr->path().generic_string().c_str());

                throw UpdateException("Updating failed, see the log for details.");
            }

            storage.insert(entry);
        }
    }
}

UpdateFetcher::DirectoryStorage UpdateFetcher::ReceiveIncludedDirectories() const
{
    DirectoryStorage directories;

    QueryResult const result = _retrieve("SELECT `path`, `state` FROM `updates_include`");
    if (!result)
        return directories;

    do
    {
        Field* fields = result->Fetch();

        std::string path = fields[0].GetString();
        if (path.substr(0, 1) == "$")
            path = _sourceDirectory.generic_string() + path.substr(1);

        Path const p(path);

        if (!is_directory(p))
        {
            TC_LOG_WARN("sql.updates", "DBUpdater: Given update include directory \"%s\" isn't existing, skipped!", p.generic_string().c_str());
            continue;
        }

        DirectoryEntry const entry = { p, AppliedFileEntry::StateConvert(fields[1].GetString()) };
        directories.push_back(entry);

        TC_LOG_TRACE("sql.updates", "Added applied file \"%s\" from remote.", p.filename().generic_string().c_str());

    } while (result->NextRow());

    return directories;
}

UpdateFetcher::AppliedFileStorage UpdateFetcher::ReceiveAppliedFiles() const
{
    AppliedFileStorage map;

    QueryResult result = _retrieve("SELECT `name`, `hash`, `state`, UNIX_TIMESTAMP(`timestamp`) FROM `updates` ORDER BY `name` ASC");
    if (!result)
        return map;

    do
    {
        Field* fields = result->Fetch();

        AppliedFileEntry const entry = { fields[0].GetString(), fields[1].GetString(),
            AppliedFileEntry::StateConvert(fields[2].GetString()), fields[3].GetUInt64() };

        map.insert(std::make_pair(entry.name, entry));
    }
    while (result->NextRow());

    return map;
}

UpdateFetcher::SQLUpdate UpdateFetcher::ReadSQLUpdate(boost::filesystem::path const& file) const
{
    std::ifstream in(file.c_str());
    WPFatal(in.is_open(), "Could not read an update file.");

    auto const start_pos = in.tellg();
    in.ignore(std::numeric_limits<std::streamsize>::max());
    auto const char_count = in.gcount();
    in.seekg(start_pos);

    SQLUpdate const update(new std::string(char_count, char{}));

    in.read(&(*update)[0], update->size());
    in.close();
    return update;
}

uint32 UpdateFetcher::Update(bool const redundancyChecks, bool const allowRehash, bool const archivedRedundancy, int32 const cleanDeadReferencesMaxCount) const
{
    LocaleFileStorage const available = GetFileList();
    AppliedFileStorage applied = ReceiveAppliedFiles();

    // Fill hash to name cache
    HashToFileNameStorage hashToName;
    for (auto entry : applied)
        hashToName.insert(std::make_pair(entry.second.hash, entry.first));

    uint32 importedUpdates = 0;

    for (auto const& availableQuery : available)
    {
        TC_LOG_DEBUG("sql.updates", "Checking update \"%s\"...", availableQuery.first.filename().generic_string().c_str());

        AppliedFileStorage::const_iterator iter = applied.find(availableQuery.first.filename().string());
        if (iter != applied.end())
        {
            // If redundancy is disabled skip it since the update is already applied.
            if (!redundancyChecks)
            {
                TC_LOG_DEBUG("sql.updates", ">> Update is already applied, skipping redundancy checks.");
                applied.erase(iter);
                continue;
            }

            // If the update is in an archived directory and is marked as archived in our database skip redundancy checks (archived updates never change).
            if (!archivedRedundancy && (iter->second.state == ARCHIVED) && (availableQuery.second == ARCHIVED))
            {
                TC_LOG_DEBUG("sql.updates", ">> Update is archived and marked as archived in database, skipping redundancy checks.");
                applied.erase(iter);
                continue;
            }
        }

        // Read update from file
        SQLUpdate const update = ReadSQLUpdate(availableQuery.first);

        // Calculate hash
        std::string const hash = CalculateHash(update);

        UpdateMode mode = MODE_APPLY;

        // Update is not in our applied list
        if (iter == applied.end())
        {
            // Catch renames (different filename but same hash)
            HashToFileNameStorage::const_iterator const hashIter = hashToName.find(hash);
            if (hashIter != hashToName.end())
            {
                // Check if the original file was removed if not we've got a problem.
                LocaleFileStorage::const_iterator localeIter;
                // Push localeIter forward
                for (localeIter = available.begin(); (localeIter != available.end()) &&
                    (localeIter->first.filename().string() != hashIter->second); ++localeIter);

                // Conflict!
                if (localeIter != available.end())
                {
                    TC_LOG_WARN("sql.updates", ">> Seems like update \"%s\" \'%s\' was renamed, but the old file is still there! " \
                        "Trade it as a new file! (Probably its an unmodified copy of file \"%s\")",
                            availableQuery.first.filename().string().c_str(), hash.substr(0, 7).c_str(),
                                localeIter->first.filename().string().c_str());
                }
                // Its save to trade the file as renamed here
                else
                {
                    TC_LOG_INFO("sql.updates", ">> Renaming update \"%s\" to \"%s\" \'%s\'.",
                        hashIter->second.c_str(), availableQuery.first.filename().string().c_str(), hash.substr(0, 7).c_str());

                    RenameEntry(hashIter->second, availableQuery.first.filename().string());
                    applied.erase(hashIter->second);
                    continue;
                }
            }
            // Apply the update if it was never seen before.
            else
            {
                TC_LOG_INFO("sql.updates", ">> Applying update \"%s\" \'%s\'...",
                    availableQuery.first.filename().string().c_str(), hash.substr(0, 7).c_str());
            }
        }
        // Rehash the update entry if it is contained in our database but with an empty hash.
        else if (allowRehash && iter->second.hash.empty())
        {
            mode = MODE_REHASH;

            TC_LOG_INFO("sql.updates", ">> Re-hashing update \"%s\" \'%s\'...", availableQuery.first.filename().string().c_str(),
                hash.substr(0, 7).c_str());
        }
        else
        {
            // If the hash of the files differs from the one stored in our database reapply the update (because it was changed).
            if (iter->second.hash != hash)
            {
                TC_LOG_INFO("sql.updates", ">> Reapplying update \"%s\" \'%s\' -> \'%s\' (it changed)...", availableQuery.first.filename().string().c_str(),
                    iter->second.hash.substr(0, 7).c_str(), hash.substr(0, 7).c_str());
            }
            else
            {
                // If the file wasn't changed and just moved update its state if necessary.
                if (iter->second.state != availableQuery.second)
                {
                    TC_LOG_DEBUG("sql.updates", ">> Updating state of \"%s\" to \'%s\'...",
                        availableQuery.first.filename().string().c_str(), AppliedFileEntry::StateConvert(availableQuery.second).c_str());

                    UpdateState(availableQuery.first.filename().string(), availableQuery.second);
                }

                TC_LOG_DEBUG("sql.updates", ">> Update is already applied and is matching hash \'%s\'.", hash.substr(0, 7).c_str());

                applied.erase(iter);
                continue;
            }
        }

        uint32 speed = 0;
        AppliedFileEntry const file = { availableQuery.first.filename().string(), hash, availableQuery.second, 0 };

        switch (mode)
        {
            case MODE_APPLY:
                speed = Apply(availableQuery.first);
                /*no break*/
            case MODE_REHASH:
                UpdateEntry(file, speed);
                break;
        }

        if (iter != applied.end())
            applied.erase(iter);

        if (mode == MODE_APPLY)
            ++importedUpdates;
    }

    // Cleanup up orphaned entries if enabled
    if (!applied.empty())
    {
        bool const doCleanup = (cleanDeadReferencesMaxCount < 0) || (applied.size() <= static_cast<size_t>(cleanDeadReferencesMaxCount));

        for (auto const& entry : applied)
        {
            TC_LOG_WARN("sql.updates", ">> File \'%s\' was applied to the database but is missing in" \
                " your update directory now!", entry.first.c_str());

            if (doCleanup)
                TC_LOG_INFO("sql.updates", "Deleting orphaned entry \'%s\'...", entry.first.c_str());
        }

        if (doCleanup)
            CleanUp(applied);
        else
        {
            TC_LOG_ERROR("sql.updates", "Cleanup is disabled! There are " SZFMTD " dirty files that were applied to your database " \
                "but are now missing in your source directory!", applied.size());
        }
    }

    return importedUpdates;
}

std::string UpdateFetcher::CalculateHash(SQLUpdate const& query) const
{
    // Calculate a Sha1 hash based on query content.
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)query->c_str(), query->length(), (unsigned char*)&digest);

    return ByteArrayToHexStr(digest, SHA_DIGEST_LENGTH);
}

uint32 UpdateFetcher::Apply(Path const& path) const
{
    using Time = std::chrono::high_resolution_clock;
    using ms = std::chrono::milliseconds;

    // Benchmark query speed
    auto const begin = Time::now();

    // Update database
    _applyFile(path);

    // Return time the query took to apply
    return std::chrono::duration_cast<ms>(Time::now() - begin).count();
}

void UpdateFetcher::UpdateEntry(AppliedFileEntry const& entry, uint32 const speed) const
{
    std::string const update = "REPLACE INTO `updates` (`name`, `hash`, `state`, `speed`) VALUES (\"" +
        entry.name + "\", \"" + entry.hash + "\", \'" + entry.GetStateAsString() + "\', " + std::to_string(speed) + ")";

    // Update database
    _apply(update);
}

void UpdateFetcher::RenameEntry(std::string const& from, std::string const& to) const
{
    // Delete target if it exists
    {
        std::string const update = "DELETE FROM `updates` WHERE `name`=\"" + to + "\"";

        // Update database
        _apply(update);
    }

    // Rename
    {
        std::string const update = "UPDATE `updates` SET `name`=\"" + to + "\" WHERE `name`=\"" + from + "\"";

        // Update database
        _apply(update);
    }
}

void UpdateFetcher::CleanUp(AppliedFileStorage const& storage) const
{
    if (storage.empty())
        return;

    std::stringstream update;
    size_t remaining = storage.size();

    update << "DELETE FROM `updates` WHERE `name` IN(";

    for (auto const& entry : storage)
    {
        update << "\"" << entry.first << "\"";
        if ((--remaining) > 0)
            update << ", ";
    }

    update << ")";

    // Update database
    _apply(update.str());
}

void UpdateFetcher::UpdateState(std::string const& name, State const state) const
{
    std::string const update = "UPDATE `updates` SET `state`=\'" + AppliedFileEntry::StateConvert(state) + "\' WHERE `name`=\"" + name + "\"";

    // Update database
    _apply(update);
}
