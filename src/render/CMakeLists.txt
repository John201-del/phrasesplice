# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: Julius Künzel <julius.kuenzel@kde.org>

#if(NOT USE_DBUS)
    list(APPEND kdenlive_SRCS
      render/renderserver.cpp)
#endif()

set(kdenlive_SRCS
  ${kdenlive_SRCS}
  render/renderrequest.cpp
  PARENT_SCOPE)
