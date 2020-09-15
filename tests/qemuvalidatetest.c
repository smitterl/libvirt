/*
 * qemuvalidatetest.c: Test the qemu domain validation
 *
 * Copyright (C) 2010-2020 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "qemu/qemu_validate.c"
#include "testutils.h"
#include "internal.h"
#include "src/conf/virconftypes.h"
#include "src/conf/domain_conf.h"
#include "src/qemu/qemu_capabilities.h"
#include "src/util/virarch.h"

static virDomainDefPtr
getDefWithUnsupportedTimerPresent(void)
{
    virDomainTimerDefPtr timer;
    virDomainDefPtr def;

    if (VIR_ALLOC(timer) < 0)
        return NULL;

    timer->present = 1;
    timer->name = VIR_DOMAIN_TIMER_NAME_TSC;

    def = virDomainDefNew();
    def->virtType = VIR_DOMAIN_VIRT_QEMU;
    def->os = (virDomainOSDef) {
        .arch = VIR_ARCH_S390X,
        .machine = (char *)"m"
    };
    def->emulator = (char *)"e";
    def->clock = (virDomainClockDef) {
        .ntimers = 1,
    };
    if (VIR_ALLOC_N(def->clock.timers, 1) < 0)
        return NULL;
    def->clock.timers[0] = timer;

    return def;
}

static int
errorTimersNotOnX86(const void *unused G_GNUC_UNUSED)
{
    int ret = 0;
    char *log;
    char expectedError[75] = "'tsc' timer is not supported for virtType=qemu arch=s390x machine=m guests";
    virDomainDefPtr def = getDefWithUnsupportedTimerPresent();
    if (qemuValidateDomainDefClockTimers(def, NULL) != -1) {
        ret = -1;
    }
    log = virTestLogContentAndReset();
    if (!strstr(log, expectedError)) {
        printf("expected : %s", expectedError);
        printf("but got : %s", virTestLogContentAndReset());
        ret = -1;
    }
    return ret;
}

static int
onlyErrorTimersNotOnX86IfPresent(const void *unused G_GNUC_UNUSED)
{
    int ret = 0;
    virDomainDefPtr def = getDefWithUnsupportedTimerPresent();
    def->clock.timers[0]->present = 0;
    if (qemuValidateDomainDefClockTimers(def, NULL) == -1) {
        ret = -1;
    }
    return ret;
}

static int
testsuite(void)
{
    int ret = 0;

#define DO_TEST(NAME) \
    if (virTestRun("Command Exec " #NAME " test", \
                   NAME, NULL) < 0) \
        ret = -1

    DO_TEST(errorTimersNotOnX86);
    DO_TEST(onlyErrorTimersNotOnX86IfPresent);

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN(testsuite);
