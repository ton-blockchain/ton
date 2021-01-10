/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/

#include "td/utils/port/user.h"

#include "td/utils/port/config.h"

#if TD_PORT_POSIX
#include <grp.h>
#include <pwd.h>
#if TD_DARWIN || TD_FREEBSD || TD_NETBSD
#include <sys/param.h>
#endif
#include <sys/types.h>
#include <unistd.h>
#endif

namespace td {

Status change_user(CSlice username, CSlice groupname) {
#if TD_PORT_POSIX
  passwd *pw = getpwnam(username.c_str());
  if (pw == nullptr) {
    return OS_ERROR(PSTRING() << "Can't find the user '" << username << "' to switch to");
  }
  uid_t uid = pw->pw_uid;
  gid_t gid = pw->pw_gid;
  if (setgroups(1, &gid) == -1) {
    return OS_ERROR("Failed to clear supplementary group list");
  }
  if (!groupname.empty()) {
    group *g = getgrnam(groupname.c_str());
    if (g == nullptr) {
      return OS_ERROR("Can't find the group to switch to");
    }
    gid = g->gr_gid;
  } else if (initgroups(username.c_str(), gid) == -1) {
    return OS_ERROR("Failed to load groups of user");
  }
  if (setgid(gid) == -1) {
    return OS_ERROR("failed to set effective group ID");
  }
  if (setuid(uid) == -1) {
    return OS_ERROR("failed to set effective user ID");
  }
  return Status::OK();
#else
  return Status::Error("Changing effective user is not supported");
#endif
}

}  // namespace td
