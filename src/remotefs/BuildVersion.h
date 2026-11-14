#pragma once

#include <QString>

namespace KateRemote
{

// This build's version: the project version and, when built from git, the
// commit ("1.5.1-650d4e3"). The daemon reports it over DBus (version()), so
// a plugin can tell a daemon still running an older install.
QString buildVersion();

}
