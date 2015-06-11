/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <fcntl.h>

#include "sd-journal.h"
#include "macro.h"
#include "journal-file.h"
#include "journal-internal.h"

int main(int argc, char *argv[]) {
        const char *fn;
        char dn[] = "/var/tmp/test-journal-flush.XXXXXX";
        JournalDirectory *dir;
        JournalFile *new_journal = NULL;
        sd_journal *j = NULL;
        unsigned n = 0;
        int r;

        assert_se(mkdtemp(dn));
        fn = "test.journal";

        r = journal_directory_open(dn, &dir);
        assert_se(r >= 0);

        r = journal_file_open(dir, fn, O_CREAT|O_RDWR, 0644, false, false, NULL, NULL, NULL, &new_journal);
        assert_se(r >= 0);

        r = sd_journal_open(&j, 0);
        assert_se(r >= 0);

        sd_journal_set_data_threshold(j, 0);

        SD_JOURNAL_FOREACH(j) {
                Object *o;
                JournalFile *f;

                f = j->current_file;
                assert_se(f && f->current_offset > 0);

                r = journal_file_move_to_object(f, OBJECT_ENTRY, f->current_offset, &o);
                assert_se(r >= 0);

                r = journal_file_copy_entry(f, new_journal, o, f->current_offset, NULL, NULL, NULL);
                assert_se(r >= 0);

                n++;
                if (n > 10000)
                        break;
        }

        sd_journal_close(j);

        journal_file_close(new_journal);

        unlinkat(dir->fd, fn, 0);
        dir = journal_directory_unref(dir);
        assert_se(rmdir(dn) == 0);

        return 0;
}
