// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef ZIP_H_48572039485720394857
#define ZIP_H_48572039485720394857

#include "abstract.h"


namespace fff
{
/* Read-only access to the contents of a ZIP archive, e.g. to sync archive => folder:
    - select the archive as left base folder:  zip:/home/user/backup.zip
    - optional sub folder inside the archive:  zip:/home/user/backup.zip|some/folder
    - variant "Mirror" (left => right): only new/changed files are extracted, obsolete files are deleted on the right side

   supported: stored + deflate, Zip64, UTF-8 names (flag bit 11 or Info-ZIP Unicode path extra field), extended timestamps
   not supported: encryption, other compression methods (reported as error when reading the file)  */
bool  acceptsItemPathPhraseZip(const Zstring& itemPathPhrase); //noexcept
AbstractPath createItemPathZip(const Zstring& itemPathPhrase); //noexcept

void zipInit();
void zipTeardown();
}

#endif //ZIP_H_48572039485720394857
