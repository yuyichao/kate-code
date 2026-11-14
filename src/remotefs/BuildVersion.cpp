#include "BuildVersion.h"

namespace KateRemote
{

QString buildVersion()
{
    return QStringLiteral(KATE_REMOTE_BUILD_VERSION);
}

}
