/*
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


#include "testutils.h"
#include "testutilsqemu.h"
#include "testutilsqemuschema.h"
#include "virstoragefile.h"
#include "virstring.h"
#include "virlog.h"
#include "qemu/qemu_block.h"
#include "qemu/qemu_qapi.h"
#include "qemu/qemu_monitor_json.h"
#include "qemu/qemu_backup.h"
#include "qemu/qemu_checkpoint.h"

#include "qemu/qemu_command.h"

#define LIBVIRT_SNAPSHOT_CONF_PRIV_H_ALLOW
#include "conf/snapshot_conf_priv.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("tests.storagetest");

struct testBackingXMLjsonXMLdata {
    int type;
    const char *xml;
    bool legacy;
    virHashTablePtr schema;
    virJSONValuePtr schemaroot;
};

static int
testBackingXMLjsonXML(const void *args)
{
    const struct testBackingXMLjsonXMLdata *data = args;
    g_autoptr(xmlDoc) xml = NULL;
    g_autoptr(xmlXPathContext) ctxt = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autoptr(virJSONValue) backendprops = NULL;
    g_autoptr(virJSONValue) wrapper = NULL;
    g_autofree char *propsstr = NULL;
    g_autofree char *protocolwrapper = NULL;
    g_autofree char *actualxml = NULL;
    g_autoptr(virStorageSource) xmlsrc = NULL;
    g_autoptr(virStorageSource) jsonsrc = NULL;
    g_auto(virBuffer) debug = VIR_BUFFER_INITIALIZER;

    if (!(xmlsrc = virStorageSourceNew()))
        return -1;

    xmlsrc->type = data->type;

    if (!(xml = virXMLParseStringCtxt(data->xml, "(test storage source XML)", &ctxt)))
        return -1;

    if (virDomainStorageSourceParse(ctxt->node, ctxt, xmlsrc, 0, NULL) < 0) {
        fprintf(stderr, "failed to parse disk source xml\n");
        return -1;
    }

    if (!(backendprops = qemuBlockStorageSourceGetBackendProps(xmlsrc,
                                                               data->legacy,
                                                               false,
                                                               false))) {
        fprintf(stderr, "failed to format disk source json\n");
        return -1;
    }

    if (!data->legacy) {
        if (testQEMUSchemaValidate(backendprops, data->schemaroot,
                                   data->schema, &debug) < 0) {
            g_autofree char *debugmsg = virBufferContentAndReset(&debug);
            g_autofree char *debugprops = virJSONValueToString(backendprops, true);

            VIR_TEST_VERBOSE("json does not conform to QAPI schema");
            VIR_TEST_DEBUG("json:\n%s\ndoes not match schema. Debug output:\n %s",
                           debugprops, NULLSTR(debugmsg));
            return -1;
        }
    }

    if (virJSONValueObjectCreate(&wrapper, "a:file", &backendprops, NULL) < 0)
        return -1;

    if (!(propsstr = virJSONValueToString(wrapper, false)))
        return -1;

    protocolwrapper = g_strdup_printf("json:%s", propsstr);

    if (virStorageSourceNewFromBackingAbsolute(protocolwrapper,
                                               &jsonsrc) < 0) {
        fprintf(stderr, "failed to parse disk json\n");
        return -1;
    }

    if (virDomainDiskSourceFormat(&buf, jsonsrc, "source", 0, false, 0, true,
                                  NULL) < 0 ||
        !(actualxml = virBufferContentAndReset(&buf))) {
        fprintf(stderr, "failed to format disk source xml\n");
        return -1;
    }

    if (STRNEQ(actualxml, data->xml)) {
        fprintf(stderr, "\n expected storage source xml:\n'%s'\n"
                        "actual storage source xml:\n%s\n"
                        "intermediate json:\n%s\n",
                        data->xml, actualxml, protocolwrapper);
        return -1;
    }

    return 0;
}

static const char *testJSONtoJSONPath = abs_srcdir "/qemublocktestdata/jsontojson/";

struct testJSONtoJSONData {
    const char *name;
    virHashTablePtr schema;
    virJSONValuePtr schemaroot;
};

static int
testJSONtoJSON(const void *args)
{
    const struct testJSONtoJSONData *data = args;
    g_auto(virBuffer) debug = VIR_BUFFER_INITIALIZER;
    g_autoptr(virJSONValue) jsonsrcout = NULL;
    g_autoptr(virStorageSource) src = NULL;
    g_autofree char *actual = NULL;
    g_autofree char *in = NULL;
    g_autofree char *infile = g_strdup_printf("%s%s-in.json", testJSONtoJSONPath,
                                              data->name);
    g_autofree char *outfile = g_strdup_printf("%s%s-out.json", testJSONtoJSONPath,
                                              data->name);

    if (virTestLoadFile(infile, &in) < 0)
        return -1;

    if (virStorageSourceNewFromBackingAbsolute(in, &src) < 0) {
        fprintf(stderr, "failed to parse disk json\n");
        return -1;
    }

    if (!(jsonsrcout = qemuBlockStorageSourceGetBackendProps(src, false, false, true))) {
        fprintf(stderr, "failed to format disk source json\n");
        return -1;
    }

    if (!(actual = virJSONValueToString(jsonsrcout, true)))
        return -1;

    if (testQEMUSchemaValidate(jsonsrcout, data->schemaroot,
                               data->schema, &debug) < 0) {
        g_autofree char *debugmsg = virBufferContentAndReset(&debug);

        VIR_TEST_VERBOSE("json does not conform to QAPI schema");
        VIR_TEST_DEBUG("json:\n%s\ndoes not match schema. Debug output:\n %s",
                       actual, NULLSTR(debugmsg));
        return -1;
    }

    return virTestCompareToFile(actual, outfile);
}


struct testQemuDiskXMLToJSONData {
    virQEMUDriverPtr driver;
    virHashTablePtr schema;
    virJSONValuePtr schemaroot;
    const char *name;
    bool fail;

    virJSONValuePtr *props;
    size_t nprops;

    virJSONValuePtr *propssrc;
    size_t npropssrc;

    virQEMUCapsPtr qemuCaps;
};


static void
testQemuDiskXMLToPropsClear(struct testQemuDiskXMLToJSONData *data)
{
    size_t i;

    for (i = 0; i < data->nprops; i++)
        virJSONValueFree(data->props[i]);

    for (i = 0; i < data->npropssrc; i++)
        virJSONValueFree(data->propssrc[i]);

    data->nprops = 0;
    VIR_FREE(data->props);
    data->npropssrc = 0;
    VIR_FREE(data->propssrc);
}


static int
testQemuDiskXMLToJSONFakeSecrets(virStorageSourcePtr src)
{
    qemuDomainStorageSourcePrivatePtr srcpriv;

    if (!src->privateData &&
        !(src->privateData = qemuDomainStorageSourcePrivateNew()))
        return -1;

    srcpriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(src);

    if (src->auth) {
        if (VIR_ALLOC(srcpriv->secinfo) < 0)
            return -1;

        srcpriv->secinfo->type = VIR_DOMAIN_SECRET_INFO_TYPE_AES;
        srcpriv->secinfo->s.aes.username = g_strdup(src->auth->username);

        srcpriv->secinfo->s.aes.alias = g_strdup_printf("%s-secalias",
                                                        NULLSTR(src->nodestorage));
    }

    if (src->encryption) {
        if (VIR_ALLOC(srcpriv->encinfo) < 0)
            return -1;

        srcpriv->encinfo->type = VIR_DOMAIN_SECRET_INFO_TYPE_AES;
        srcpriv->encinfo->s.aes.alias = g_strdup_printf("%s-encalias",
                                                        NULLSTR(src->nodeformat));
    }

    return 0;
}


static const char *testQemuDiskXMLToJSONPath = abs_srcdir "/qemublocktestdata/xml2json/";

static int
testQemuDiskXMLToProps(const void *opaque)
{
    struct testQemuDiskXMLToJSONData *data = (void *) opaque;
    g_autoptr(virDomainDef) vmdef = NULL;
    virDomainDiskDefPtr disk = NULL;
    virStorageSourcePtr n;
    virJSONValuePtr formatProps = NULL;
    virJSONValuePtr storageProps = NULL;
    g_autoptr(virJSONValue) storageSrcOnlyProps = NULL;
    char *xmlpath = NULL;
    char *xmlstr = NULL;
    int ret = -1;

    xmlpath = g_strdup_printf("%s%s.xml", testQemuDiskXMLToJSONPath, data->name);

    if (virTestLoadFile(xmlpath, &xmlstr) < 0)
        goto cleanup;

    /* qemu stores node names in the status XML portion */
    if (!(disk = virDomainDiskDefParse(xmlstr, data->driver->xmlopt,
                                       VIR_DOMAIN_DEF_PARSE_STATUS)))
        goto cleanup;

    if (!(vmdef = virDomainDefNew()) ||
        virDomainDiskInsert(vmdef, disk) < 0)
        goto cleanup;

    if (qemuCheckDiskConfig(disk, vmdef, data->qemuCaps) < 0 ||
        qemuDomainDeviceDefValidateDisk(disk, data->qemuCaps) < 0) {
        VIR_TEST_VERBOSE("invalid configuration for disk");
        goto cleanup;
    }

    for (n = disk->src; virStorageSourceIsBacking(n); n = n->backingStore) {
        if (testQemuDiskXMLToJSONFakeSecrets(n) < 0)
            goto cleanup;

        if (qemuDomainValidateStorageSource(n, data->qemuCaps) < 0)
            goto cleanup;

        qemuDomainPrepareDiskSourceData(disk, n);

        if (!(formatProps = qemuBlockStorageSourceGetBlockdevProps(n, n->backingStore)) ||
            !(storageSrcOnlyProps = qemuBlockStorageSourceGetBackendProps(n, false, true, true)) ||
            !(storageProps = qemuBlockStorageSourceGetBackendProps(n, false, false, true))) {
            if (!data->fail) {
                VIR_TEST_VERBOSE("failed to generate qemu blockdev props");
                goto cleanup;
            }
        } else if (data->fail) {
            VIR_TEST_VERBOSE("qemu blockdev props should have failed");
            goto cleanup;
        }

        if (VIR_APPEND_ELEMENT(data->props, data->nprops, formatProps) < 0 ||
            VIR_APPEND_ELEMENT(data->props, data->nprops, storageProps) < 0 ||
            VIR_APPEND_ELEMENT(data->propssrc, data->npropssrc, storageSrcOnlyProps) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    virJSONValueFree(formatProps);
    virJSONValueFree(storageProps);
    VIR_FREE(xmlpath);
    VIR_FREE(xmlstr);
    return ret;
}


static int
testQemuDiskXMLToPropsValidateSchema(const void *opaque)
{
    struct testQemuDiskXMLToJSONData *data = (void *) opaque;
    virBuffer debug = VIR_BUFFER_INITIALIZER;
    char *propsstr = NULL;
    char *debugmsg = NULL;
    int ret = 0;
    size_t i;

    if (data->fail)
        return EXIT_AM_SKIP;

    for (i = 0; i < data->nprops; i++) {
        if (testQEMUSchemaValidate(data->props[i], data->schemaroot,
                                   data->schema, &debug) < 0) {
            debugmsg = virBufferContentAndReset(&debug);
            propsstr = virJSONValueToString(data->props[i], true);
            VIR_TEST_VERBOSE("json does not conform to QAPI schema");
            VIR_TEST_DEBUG("json:\n%s\ndoes not match schema. Debug output:\n %s",
                           propsstr, NULLSTR(debugmsg));
            VIR_FREE(debugmsg);
            VIR_FREE(propsstr);
            ret = -1;
        }

        virBufferFreeAndReset(&debug);
    }

    for (i = 0; i < data->npropssrc; i++) {
        if (testQEMUSchemaValidate(data->propssrc[i], data->schemaroot,
                                   data->schema, &debug) < 0) {
            debugmsg = virBufferContentAndReset(&debug);
            propsstr = virJSONValueToString(data->propssrc[i], true);
            VIR_TEST_VERBOSE("json does not conform to QAPI schema");
            VIR_TEST_DEBUG("json:\n%s\ndoes not match schema. Debug output:\n %s",
                           propsstr, NULLSTR(debugmsg));
            VIR_FREE(debugmsg);
            VIR_FREE(propsstr);
            ret = -1;
        }

        virBufferFreeAndReset(&debug);
    }

    return ret;
}


static int
testQemuDiskXMLToPropsValidateFile(const void *opaque)
{
    struct testQemuDiskXMLToJSONData *data = (void *) opaque;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *jsonpath = NULL;
    char *actual = NULL;
    int ret = -1;
    size_t i;

    if (data->fail)
        return EXIT_AM_SKIP;

    jsonpath = g_strdup_printf("%s%s.json", testQemuDiskXMLToJSONPath, data->name);

    for (i = 0; i < data->nprops; i++) {
        char *jsonstr;

        if (!(jsonstr = virJSONValueToString(data->props[i], true)))
            goto cleanup;

        virBufferAdd(&buf, jsonstr, -1);
        VIR_FREE(jsonstr);
    }

    actual = virBufferContentAndReset(&buf);

    ret = virTestCompareToFile(actual, jsonpath);

 cleanup:
    VIR_FREE(jsonpath);
    VIR_FREE(actual);
    return ret;
}


struct testQemuImageCreateData {
    const char *name;
    const char *backingname;
    virHashTablePtr schema;
    virJSONValuePtr schemaroot;
    virQEMUDriverPtr driver;
    virQEMUCapsPtr qemuCaps;
};

static const char *testQemuImageCreatePath = abs_srcdir "/qemublocktestdata/imagecreate/";

static virStorageSourcePtr
testQemuImageCreateLoadDiskXML(const char *name,
                               virDomainXMLOptionPtr xmlopt)

{
    virDomainSnapshotDiskDefPtr diskdef = NULL;
    g_autoptr(xmlDoc) doc = NULL;
    g_autoptr(xmlXPathContext) ctxt = NULL;
    xmlNodePtr node;
    g_autofree char *xmlpath = NULL;
    virStorageSourcePtr ret = NULL;

    xmlpath = g_strdup_printf("%s%s.xml", testQemuImageCreatePath, name);

    if (!(doc = virXMLParseFileCtxt(xmlpath, &ctxt)))
        return NULL;

    if (!(node = virXPathNode("//disk", ctxt))) {
        VIR_TEST_VERBOSE("failed to find <source> element\n");
        return NULL;
    }

    if (VIR_ALLOC(diskdef) < 0)
        return NULL;

    if (virDomainSnapshotDiskDefParseXML(node, ctxt, diskdef,
                                         VIR_DOMAIN_DEF_PARSE_STATUS,
                                         xmlopt) == 0)
        ret = g_steal_pointer(&diskdef->src);

    virDomainSnapshotDiskDefFree(diskdef);
    return ret;
}


static int
testQemuImageCreate(const void *opaque)
{
    struct testQemuImageCreateData *data = (void *) opaque;
    g_autoptr(virJSONValue) protocolprops = NULL;
    g_autoptr(virJSONValue) formatprops = NULL;
    g_autoptr(virStorageSource) src = NULL;
    g_auto(virBuffer) debug = VIR_BUFFER_INITIALIZER;
    g_auto(virBuffer) actualbuf = VIR_BUFFER_INITIALIZER;
    g_autofree char *jsonprotocol = NULL;
    g_autofree char *jsonformat = NULL;
    g_autofree char *actual = NULL;
    g_autofree char *jsonpath = NULL;

    if (!(src = testQemuImageCreateLoadDiskXML(data->name, data->driver->xmlopt)))
        return -1;

    if (data->backingname &&
        !(src->backingStore = testQemuImageCreateLoadDiskXML(data->backingname,
                                                             data->driver->xmlopt)))
        return -1;

    if (testQemuDiskXMLToJSONFakeSecrets(src) < 0)
        return -1;

    /* fake some sizes */
    src->capacity = UINT_MAX * 2ULL;
    src->physical = UINT_MAX + 1ULL;

    if (qemuDomainValidateStorageSource(src, data->qemuCaps) < 0)
        return -1;

    if (qemuBlockStorageSourceCreateGetStorageProps(src, &protocolprops) < 0)
        return -1;

    if (qemuBlockStorageSourceCreateGetFormatProps(src, src->backingStore, &formatprops) < 0)
        return -1;

    if (formatprops) {
        if (!(jsonformat = virJSONValueToString(formatprops, true)))
            return -1;

        if (testQEMUSchemaValidate(formatprops, data->schemaroot, data->schema,
                                   &debug) < 0) {
            g_autofree char *debugmsg = virBufferContentAndReset(&debug);
            VIR_TEST_VERBOSE("blockdev-create format json does not conform to QAPI schema");
            VIR_TEST_DEBUG("json:\n%s\ndoes not match schema. Debug output:\n %s",
                           jsonformat, NULLSTR(debugmsg));
            return -1;
        }
        virBufferFreeAndReset(&debug);
    }

    if (protocolprops) {
        if (!(jsonprotocol = virJSONValueToString(protocolprops, true)))
            return -1;

        if (testQEMUSchemaValidate(protocolprops, data->schemaroot, data->schema,
                                   &debug) < 0) {
            g_autofree char *debugmsg = virBufferContentAndReset(&debug);
            VIR_TEST_VERBOSE("blockdev-create protocol json does not conform to QAPI schema");
            VIR_TEST_DEBUG("json:\n%s\ndoes not match schema. Debug output:\n %s",
                           jsonprotocol, NULLSTR(debugmsg));
            return -1;
        }
        virBufferFreeAndReset(&debug);
    }

    virBufferStrcat(&actualbuf, "protocol:\n", NULLSTR(jsonprotocol),
                    "\nformat:\n", NULLSTR(jsonformat), NULL);
    virBufferTrim(&actualbuf, "\n");
    virBufferAddLit(&actualbuf, "\n");

    jsonpath = g_strdup_printf("%s%s.json", testQemuImageCreatePath, data->name);

    if (!(actual = virBufferContentAndReset(&actualbuf)))
        return -1;

    return virTestCompareToFile(actual, jsonpath);
}


static int
testQemuDiskXMLToPropsValidateFileSrcOnly(const void *opaque)
{
    struct testQemuDiskXMLToJSONData *data = (void *) opaque;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autofree char *jsonpath = NULL;
    g_autofree char *actual = NULL;
    size_t i;

    if (data->fail)
        return EXIT_AM_SKIP;

    jsonpath = g_strdup_printf("%s%s-srconly.json", testQemuDiskXMLToJSONPath,
                               data->name);

    for (i = 0; i < data->npropssrc; i++) {
        g_autofree char *jsonstr = NULL;

        if (!(jsonstr = virJSONValueToString(data->propssrc[i], true)))
            return -1;

        virBufferAdd(&buf, jsonstr, -1);
    }

    actual = virBufferContentAndReset(&buf);

    return virTestCompareToFile(actual, jsonpath);
}


static const char *bitmapDetectPrefix = "qemublocktestdata/bitmap/";

static void
testQemuDetectBitmapsWorker(virHashTablePtr nodedata,
                            const char *nodename,
                            virBufferPtr buf)
{
    qemuBlockNamedNodeDataPtr data;
    size_t i;

    if (!(data = virHashLookup(nodedata, nodename)))
        return;

    virBufferAsprintf(buf, "%s:\n", nodename);
    virBufferAdjustIndent(buf, 1);

    for (i = 0; i < data->nbitmaps; i++) {
        qemuBlockNamedNodeDataBitmapPtr bitmap = data->bitmaps[i];

        virBufferAsprintf(buf, "%8s: record:%d busy:%d persist:%d inconsist:%d gran:%llu dirty:%llu\n",
                          bitmap->name, bitmap->recording, bitmap->busy,
                          bitmap->persistent, bitmap->inconsistent,
                          bitmap->granularity, bitmap->dirtybytes);
    }

    virBufferAdjustIndent(buf, -1);
}


static int
testQemuDetectBitmaps(const void *opaque)
{
    const char *name = opaque;
    g_autoptr(virJSONValue) nodedatajson = NULL;
    g_autoptr(virHashTable) nodedata = NULL;
    g_autofree char *actual = NULL;
    g_autofree char *expectpath = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    expectpath = g_strdup_printf("%s/%s%s.out", abs_srcdir,
                                 bitmapDetectPrefix, name);

    if (!(nodedatajson = virTestLoadFileJSON(bitmapDetectPrefix, name,
                                             ".json", NULL)))
        return -1;

    if (!(nodedata = qemuMonitorJSONBlockGetNamedNodeDataJSON(nodedatajson))) {
        VIR_TEST_VERBOSE("failed to load nodedata JSON");
        return -1;
    }

    /* we detect for the first 30 nodenames for simplicity */
    for (i = 0; i < 30; i++) {
        g_autofree char *nodename = g_strdup_printf("libvirt-%zu-format", i);

        testQemuDetectBitmapsWorker(nodedata, nodename, &buf);
    }

    actual = virBufferContentAndReset(&buf);

    return virTestCompareToFile(actual, expectpath);
}


static virStorageSourcePtr
testQemuBackupIncrementalBitmapCalculateGetFakeImage(size_t idx)
{
   virStorageSourcePtr ret;

   if (!(ret = virStorageSourceNew()))
       abort();

   ret->id = idx;
   ret->type = VIR_STORAGE_TYPE_FILE;
   ret->format = VIR_STORAGE_FILE_QCOW2;
   ret->path = g_strdup_printf("/image%zu", idx);
   ret->nodestorage = g_strdup_printf("libvirt-%zu-storage", idx);
   ret->nodeformat = g_strdup_printf("libvirt-%zu-format", idx);

   return ret;
}


static virStorageSourcePtr
testQemuBackupIncrementalBitmapCalculateGetFakeChain(void)
{
    virStorageSourcePtr ret;
    virStorageSourcePtr n;
    size_t i;

    n = ret = testQemuBackupIncrementalBitmapCalculateGetFakeImage(1);

    for (i = 2; i < 6; i++) {
        n->backingStore = testQemuBackupIncrementalBitmapCalculateGetFakeImage(i);
        n = n->backingStore;
    }

    return ret;
}


static virStorageSourcePtr
testQemuBitmapGetFakeChainEntry(virStorageSourcePtr src,
                                size_t idx)
{
    virStorageSourcePtr n;

    for (n = src; n; n = n->backingStore) {
        if (n->id == idx)
            return n;
    }

    return NULL;
}


typedef virDomainMomentDefPtr testMomentList;

static void
testMomentListFree(testMomentList *list)
{
    testMomentList *tmp = list;

    if (!list)
        return;

    while (*tmp) {
        virObjectUnref(*tmp);
        tmp++;
    }

    g_free(list);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(testMomentList, testMomentListFree);

static virDomainMomentDefPtr
testQemuBackupGetIncrementalMoment(const char *name)
{
    virDomainCheckpointDefPtr checkpoint = NULL;

    if (!(checkpoint = virDomainCheckpointDefNew()))
        abort();

    checkpoint->parent.name = g_strdup(name);

    return (virDomainMomentDefPtr) checkpoint;
}


static virDomainMomentDefPtr *
testQemuBackupGetIncremental(const char *incFrom)
{
    const char *checkpoints[] = {"current", "d", "c", "b", "a"};
    virDomainMomentDefPtr *incr;
    size_t i;

    incr = g_new0(virDomainMomentDefPtr, G_N_ELEMENTS(checkpoints) + 1);

    for (i = 0; i < G_N_ELEMENTS(checkpoints); i++) {
        incr[i] = testQemuBackupGetIncrementalMoment(checkpoints[i]);

        if (STREQ(incFrom, checkpoints[i]))
            break;
    }

    return incr;
}

static const char *backupDataPrefix = "qemublocktestdata/backupmerge/";

struct testQemuBackupIncrementalBitmapCalculateData {
    const char *name;
    virStorageSourcePtr chain;
    const char *incremental;
    const char *nodedatafile;
};


static int
testQemuBackupIncrementalBitmapCalculate(const void *opaque)
{
    const struct testQemuBackupIncrementalBitmapCalculateData *data = opaque;
    g_autoptr(virJSONValue) nodedatajson = NULL;
    g_autoptr(virHashTable) nodedata = NULL;
    g_autoptr(virJSONValue) mergebitmaps = NULL;
    g_autofree char *actual = NULL;
    g_autofree char *expectpath = NULL;
    g_autoptr(testMomentList) incremental = NULL;

    expectpath = g_strdup_printf("%s/%s%s-out.json", abs_srcdir,
                                 backupDataPrefix, data->name);

    if (!(nodedatajson = virTestLoadFileJSON(bitmapDetectPrefix, data->nodedatafile,
                                             ".json", NULL)))
        return -1;

    if (!(nodedata = qemuMonitorJSONBlockGetNamedNodeDataJSON(nodedatajson))) {
        VIR_TEST_VERBOSE("failed to load nodedata JSON\n");
        return -1;
    }

    incremental = testQemuBackupGetIncremental(data->incremental);

    if (!(mergebitmaps = qemuBackupDiskPrepareOneBitmapsChain(incremental,
                                                              data->chain,
                                                              nodedata,
                                                              "testdisk"))) {
        VIR_TEST_VERBOSE("failed to calculate merged bitmaps");
        return -1;
    }

    if (!(actual = virJSONValueToString(mergebitmaps, true)))
        return -1;

    return virTestCompareToFile(actual, expectpath);
}


static const char *checkpointDeletePrefix = "qemublocktestdata/checkpointdelete/";

struct testQemuCheckpointDeleteMergeData {
    const char *name;
    virStorageSourcePtr chain;
    const char *deletebitmap;
    const char *parentbitmap;
    const char *nodedatafile;
};


static int
testQemuCheckpointDeleteMerge(const void *opaque)
{
    const struct testQemuCheckpointDeleteMergeData *data = opaque;
    g_autofree char *actual = NULL;
    g_autofree char *expectpath = NULL;
    g_autoptr(virJSONValue) actions = NULL;
    g_autoptr(virJSONValue) nodedatajson = NULL;
    g_autoptr(virHashTable) nodedata = NULL;
    g_autoptr(GSList) reopenimages = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    GSList *tmp;

    expectpath = g_strdup_printf("%s/%s%s-out.json", abs_srcdir,
                                 checkpointDeletePrefix, data->name);

    if (!(nodedatajson = virTestLoadFileJSON(bitmapDetectPrefix, data->nodedatafile,
                                             ".json", NULL)))
        return -1;

    if (!(nodedata = qemuMonitorJSONBlockGetNamedNodeDataJSON(nodedatajson))) {
        VIR_TEST_VERBOSE("failed to load nodedata JSON\n");
        return -1;
    }

    actions = virJSONValueNewArray();

    if (qemuCheckpointDiscardDiskBitmaps(data->chain,
                                         nodedata,
                                         data->deletebitmap,
                                         data->parentbitmap,
                                         actions,
                                         "testdisk",
                                         &reopenimages) < 0) {
        VIR_TEST_VERBOSE("failed to generate checkpoint delete transaction\n");
        return -1;
    }

    if (virJSONValueToBuffer(actions, &buf, true) < 0)
        return -1;

    if (reopenimages) {
        virBufferAddLit(&buf, "reopen nodes:\n");

        for (tmp = reopenimages; tmp; tmp = tmp->next) {
            virStorageSourcePtr src = tmp->data;
            virBufferAsprintf(&buf, "%s\n", src->nodeformat);
        }
    }

    actual = virBufferContentAndReset(&buf);

    return virTestCompareToFile(actual, expectpath);
}


struct testQemuBlockBitmapValidateData {
    const char *name;
    const char *bitmapname;
    virStorageSourcePtr chain;
    bool expect;
};

static int
testQemuBlockBitmapValidate(const void *opaque)
{
    const struct testQemuBlockBitmapValidateData *data = opaque;
    g_autoptr(virJSONValue) nodedatajson = NULL;
    g_autoptr(virHashTable) nodedata = NULL;
    bool actual;

    if (!(nodedatajson = virTestLoadFileJSON(bitmapDetectPrefix, data->name,
                                             ".json", NULL)))
        return -1;

    if (!(nodedata = qemuMonitorJSONBlockGetNamedNodeDataJSON(nodedatajson))) {
        VIR_TEST_VERBOSE("failed to load nodedata JSON\n");
        return -1;
    }

    actual = qemuBlockBitmapChainIsValid(data->chain, data->bitmapname, nodedata);

    if (actual != data->expect) {
        VIR_TEST_VERBOSE("expected rv:'%d' actual rv:'%d'\n", data->expect, actual);
        return -1;
    }

    return 0;
}


static const char *blockcopyPrefix = "qemublocktestdata/bitmapblockcopy/";

struct testQemuBlockBitmapBlockcopyData {
    const char *name;
    bool shallow;
    virStorageSourcePtr chain;
    const char *nodedatafile;
};


static int
testQemuBlockBitmapBlockcopy(const void *opaque)
{
    const struct testQemuBlockBitmapBlockcopyData *data = opaque;
    g_autofree char *actual = NULL;
    g_autofree char *expectpath = NULL;
    g_autoptr(virJSONValue) actions = NULL;
    g_autoptr(virJSONValue) nodedatajson = NULL;
    g_autoptr(virHashTable) nodedata = NULL;
    g_autoptr(virStorageSource) fakemirror = virStorageSourceNew();

    if (!fakemirror)
        return -1;

    fakemirror->nodeformat = g_strdup("mirror-format-node");

    expectpath = g_strdup_printf("%s/%s%s-out.json", abs_srcdir,
                                 blockcopyPrefix, data->name);

    if (!(nodedatajson = virTestLoadFileJSON(bitmapDetectPrefix, data->nodedatafile,
                                             ".json", NULL)))
        return -1;

    if (!(nodedata = qemuMonitorJSONBlockGetNamedNodeDataJSON(nodedatajson))) {
        VIR_TEST_VERBOSE("failed to load nodedata JSON\n");
        return -1;
    }

    if (qemuBlockBitmapsHandleBlockcopy(data->chain, fakemirror, nodedata,
                                        data->shallow, &actions) < 0)
        return -1;

    if (actions &&
        !(actual = virJSONValueToString(actions, true)))
        return -1;

    return virTestCompareToFile(actual, expectpath);
}

static const char *blockcommitPrefix = "qemublocktestdata/bitmapblockcommit/";

struct testQemuBlockBitmapBlockcommitData {
    const char *name;
    virStorageSourcePtr top;
    virStorageSourcePtr base;
    virStorageSourcePtr chain;
    const char *nodedatafile;
};


static int
testQemuBlockBitmapBlockcommit(const void *opaque)
{
    const struct testQemuBlockBitmapBlockcommitData *data = opaque;

    g_autofree char *actual = NULL;
    g_autofree char *expectpath = NULL;
    g_autoptr(virJSONValue) actionsDisable = NULL;
    g_autoptr(virJSONValue) actionsMerge = NULL;
    g_autoptr(virJSONValue) nodedatajson = NULL;
    g_autoptr(virHashTable) nodedata = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    VIR_AUTOSTRINGLIST bitmapsDisable = NULL;

    expectpath = g_strdup_printf("%s/%s%s", abs_srcdir,
                                 blockcommitPrefix, data->name);

    if (!(nodedatajson = virTestLoadFileJSON(bitmapDetectPrefix, data->nodedatafile,
                                             ".json", NULL)))
        return -1;

    if (!(nodedata = qemuMonitorJSONBlockGetNamedNodeDataJSON(nodedatajson))) {
        VIR_TEST_VERBOSE("failed to load nodedata JSON\n");
        return -1;
    }

    if (qemuBlockBitmapsHandleCommitStart(data->top, data->base, nodedata,
                                          &actionsDisable, &bitmapsDisable) < 0)
        return -1;

    virBufferAddLit(&buf, "pre job bitmap disable:\n");

    if (actionsDisable &&
        virJSONValueToBuffer(actionsDisable, &buf, true) < 0)
        return -1;

    virBufferAddLit(&buf, "merge bitmpas:\n");

    if (qemuBlockBitmapsHandleCommitFinish(data->top, data->base, nodedata,
                                           &actionsMerge, bitmapsDisable) < 0)
        return -1;

    if (actionsMerge &&
        virJSONValueToBuffer(actionsMerge, &buf, true) < 0)
        return -1;

    actual = virBufferContentAndReset(&buf);

    return virTestCompareToFile(actual, expectpath);
}


static int
mymain(void)
{
    int ret = 0;
    virQEMUDriver driver;
    struct testBackingXMLjsonXMLdata xmljsonxmldata;
    struct testQemuDiskXMLToJSONData diskxmljsondata;
    struct testJSONtoJSONData jsontojsondata;
    struct testQemuImageCreateData imagecreatedata;
    struct testQemuBackupIncrementalBitmapCalculateData backupbitmapcalcdata;
    struct testQemuCheckpointDeleteMergeData checkpointdeletedata;
    struct testQemuBlockBitmapValidateData blockbitmapvalidatedata;
    struct testQemuBlockBitmapBlockcopyData blockbitmapblockcopydata;
    struct testQemuBlockBitmapBlockcommitData blockbitmapblockcommitdata;
    char *capslatest_x86_64 = NULL;
    virQEMUCapsPtr caps_x86_64 = NULL;
    g_autoptr(virHashTable) qmp_schema_x86_64 = NULL;
    virJSONValuePtr qmp_schemaroot_x86_64_blockdev_add = NULL;
    g_autoptr(virStorageSource) bitmapSourceChain = NULL;

    if (qemuTestDriverInit(&driver) < 0)
        return EXIT_FAILURE;

    bitmapSourceChain = testQemuBackupIncrementalBitmapCalculateGetFakeChain();

    diskxmljsondata.driver = &driver;
    imagecreatedata.driver = &driver;

    if (!(capslatest_x86_64 = testQemuGetLatestCapsForArch("x86_64", "xml")))
        return EXIT_FAILURE;

    VIR_TEST_VERBOSE("\nlatest caps x86_64: %s", capslatest_x86_64);

    if (!(caps_x86_64 = qemuTestParseCapabilitiesArch(virArchFromString("x86_64"),
                                                      capslatest_x86_64)))
        return EXIT_FAILURE;

    diskxmljsondata.qemuCaps = caps_x86_64;
    imagecreatedata.qemuCaps = caps_x86_64;

    if (!(qmp_schema_x86_64 = testQEMUSchemaLoad("x86_64"))) {
        ret = -1;
        goto cleanup;
    }

    if (virQEMUQAPISchemaPathGet("blockdev-add/arg-type",
                                 qmp_schema_x86_64,
                                 &qmp_schemaroot_x86_64_blockdev_add) < 0 ||
        !qmp_schemaroot_x86_64_blockdev_add) {
        VIR_TEST_VERBOSE("failed to find schema entry for blockdev-add");
        ret = -1;
        goto cleanup;
    }

    virTestCounterReset("qemu storage source xml->json->xml ");

#define TEST_JSON_FORMAT(tpe, xmlstr) \
    do { \
        xmljsonxmldata.type = tpe; \
        xmljsonxmldata.xml = xmlstr; \
        xmljsonxmldata.legacy = true; \
        if (virTestRun(virTestCounterNext(), testBackingXMLjsonXML, \
                       &xmljsonxmldata) < 0) \
        xmljsonxmldata.legacy = false; \
        if (virTestRun(virTestCounterNext(), testBackingXMLjsonXML, \
                       &xmljsonxmldata) < 0) \
            ret = -1; \
    } while (0)

#define TEST_JSON_FORMAT_NET(xmlstr) \
    TEST_JSON_FORMAT(VIR_STORAGE_TYPE_NETWORK, xmlstr)

    xmljsonxmldata.schema = qmp_schema_x86_64;
    xmljsonxmldata.schemaroot = qmp_schemaroot_x86_64_blockdev_add;

    TEST_JSON_FORMAT(VIR_STORAGE_TYPE_FILE, "<source file='/path/to/file'/>\n");

    /* type VIR_STORAGE_TYPE_BLOCK is not tested since it parses back to 'file' */
    /* type VIR_STORAGE_TYPE_DIR it is a 'format' driver in qemu */

    TEST_JSON_FORMAT_NET("<source protocol='http' name=''>\n"
                         "  <host name='example.com' port='80'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='http' name='file'>\n"
                         "  <host name='example.com' port='80'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='https' name='file'>\n"
                         "  <host name='example.com' port='432'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='https' name='file'>\n"
                         "  <host name='example.com' port='432'/>\n"
                         "  <ssl verify='no'/>\n"
                         "  <readahead size='1024'/>\n"
                         "  <timeout seconds='1337'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='gluster' name='vol/file'>\n"
                         "  <host name='example.com' port='24007'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='gluster' name='testvol/img.qcow2'>\n"
                         "  <host name='example.com' port='1234'/>\n"
                         "  <host transport='unix' socket='/path/socket'/>\n"
                         "  <host name='example.com' port='24007'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='nbd'>\n"
                         "  <host transport='unix' socket='/path/to/socket'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='nbd' name='blah'>\n"
                         "  <host name='example.org' port='6000'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='nbd'>\n"
                         "  <host name='example.org' port='6000'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='ssh' name='blah'>\n"
                         "  <host name='example.org' port='6000'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='rbd' name='libvirt/test'>\n"
                         "  <host name='example.com' port='1234'/>\n"
                         "  <host name='example2.com'/>\n"
                         "  <snapshot name='snapshotname'/>\n"
                         "  <config file='/path/to/conf'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='iscsi' name='iqn.2016-12.com.virttest:emulated-iscsi-noauth.target/0'>\n"
                         "  <host name='test.org' port='3260'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='iscsi' name='iqn.2016-12.com.virttest:emulated-iscsi-noauth.target/6'>\n"
                         "  <host name='test.org' port='1234'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='sheepdog' name='test'>\n"
                         "  <host name='example.com' port='321'/>\n"
                         "</source>\n");
    TEST_JSON_FORMAT_NET("<source protocol='vxhs' name='c6718f6b-0401-441d-a8c3-1f0064d75ee0'>\n"
                         "  <host name='example.com' port='9999'/>\n"
                         "</source>\n");

#define TEST_DISK_TO_JSON_FULL(nme, fl) \
    do { \
        diskxmljsondata.name = nme; \
        diskxmljsondata.props = NULL; \
        diskxmljsondata.nprops = 0; \
        diskxmljsondata.propssrc = NULL; \
        diskxmljsondata.npropssrc = 0; \
        diskxmljsondata.fail = fl; \
        if (virTestRun("disk xml to props " nme, testQemuDiskXMLToProps, \
                       &diskxmljsondata) < 0) \
            ret = -1; \
        if (virTestRun("disk xml to props validate schema " nme, \
                       testQemuDiskXMLToPropsValidateSchema, &diskxmljsondata) < 0) \
            ret = -1; \
        if (virTestRun("disk xml to props validate file " nme, \
                       testQemuDiskXMLToPropsValidateFile,  &diskxmljsondata) < 0) \
            ret = -1; \
        if (virTestRun("disk xml to props source only validate file " nme, \
                       testQemuDiskXMLToPropsValidateFileSrcOnly,  &diskxmljsondata) < 0) \
            ret = -1; \
        testQemuDiskXMLToPropsClear(&diskxmljsondata); \
    } while (0)

#define TEST_DISK_TO_JSON(nme) TEST_DISK_TO_JSON_FULL(nme, false)

    diskxmljsondata.schema = qmp_schema_x86_64;
    diskxmljsondata.schemaroot = qmp_schemaroot_x86_64_blockdev_add;

    TEST_DISK_TO_JSON_FULL("nodename-long-format", true);
    TEST_DISK_TO_JSON_FULL("nodename-long-protocol", true);

    TEST_DISK_TO_JSON("file-raw-noopts");
    TEST_DISK_TO_JSON("file-bochs-noopts");
    TEST_DISK_TO_JSON("file-cloop-noopts");
    TEST_DISK_TO_JSON("file-dmg-noopts");
    TEST_DISK_TO_JSON("file-ploop-noopts");
    TEST_DISK_TO_JSON("file-vdi-noopts");
    TEST_DISK_TO_JSON("file-vhd-noopts");
    TEST_DISK_TO_JSON("file-vpc-noopts");

    TEST_DISK_TO_JSON("file-backing_basic-noopts");
    TEST_DISK_TO_JSON("dir-fat-readonly");
    TEST_DISK_TO_JSON("dir-fat-floppy");
    TEST_DISK_TO_JSON("file-raw-aio_native");
    TEST_DISK_TO_JSON("file-backing_basic-aio_threads");
    TEST_DISK_TO_JSON("file-raw-luks");
    TEST_DISK_TO_JSON("file-qcow2-backing-chain-noopts");
    TEST_DISK_TO_JSON("file-qcow2-backing-chain-unterminated");
    TEST_DISK_TO_JSON("file-qcow2-backing-chain-encryption");
    TEST_DISK_TO_JSON("network-qcow2-backing-chain-encryption_auth");

    TEST_DISK_TO_JSON("file-backing_basic-unmap");
    TEST_DISK_TO_JSON("file-backing_basic-unmap-detect");
    TEST_DISK_TO_JSON("file-backing_basic-unmap-ignore");
    TEST_DISK_TO_JSON("file-backing_basic-detect");

    TEST_DISK_TO_JSON("file-backing_basic-cache-none");
    TEST_DISK_TO_JSON("file-backing_basic-cache-writethrough");
    TEST_DISK_TO_JSON("file-backing_basic-cache-writeback");
    TEST_DISK_TO_JSON("file-backing_basic-cache-directsync");
    TEST_DISK_TO_JSON("file-backing_basic-cache-unsafe");
    TEST_DISK_TO_JSON("network-qcow2-backing-chain-cache-unsafe");
    TEST_DISK_TO_JSON("dir-fat-cache");
    TEST_DISK_TO_JSON("network-nbd-tls");

    TEST_DISK_TO_JSON("block-raw-noopts");
    TEST_DISK_TO_JSON("block-raw-reservations");

#define TEST_JSON_TO_JSON(nme) \
    do { \
        jsontojsondata.name = nme; \
        if (virTestRun("JSON to JSON " nme, testJSONtoJSON, \
                       &jsontojsondata) < 0) \
            ret = -1; \
    } while (0)

    jsontojsondata.schema = qmp_schema_x86_64;
    jsontojsondata.schemaroot = qmp_schemaroot_x86_64_blockdev_add;

    TEST_JSON_TO_JSON("curl-libguestfs");
    TEST_JSON_TO_JSON("ssh-passthrough-libguestfs");

#define TEST_IMAGE_CREATE(testname, testbacking) \
    do { \
        imagecreatedata.name = testname; \
        imagecreatedata.backingname = testbacking; \
        if (virTestRun("image create xml to props " testname, testQemuImageCreate, \
                       &imagecreatedata) < 0) \
            ret = -1; \
    } while (0)

    imagecreatedata.schema = qmp_schema_x86_64;

    if (virQEMUQAPISchemaPathGet("blockdev-create/arg-type/options",
                                 imagecreatedata.schema,
                                 &imagecreatedata.schemaroot) < 0 ||
        !imagecreatedata.schemaroot) {
        VIR_TEST_VERBOSE("failed to find schema entry for blockdev-create\n");
        ret = -1;
        goto cleanup;
    }

    TEST_IMAGE_CREATE("raw", NULL);
    TEST_IMAGE_CREATE("raw-nbd", NULL);
    TEST_IMAGE_CREATE("luks-noopts", NULL);
    TEST_IMAGE_CREATE("luks-encopts", NULL);
    TEST_IMAGE_CREATE("qcow2", NULL);
    TEST_IMAGE_CREATE("qcow2-luks-noopts", NULL);
    TEST_IMAGE_CREATE("qcow2-luks-encopts", NULL);
    TEST_IMAGE_CREATE("qcow2-backing-raw", "raw");
    TEST_IMAGE_CREATE("qcow2-backing-raw-nbd", "raw-nbd");
    TEST_IMAGE_CREATE("qcow2-backing-luks", "luks-noopts");
    TEST_IMAGE_CREATE("qcow2-luks-encopts-backing", "qcow2");
    TEST_IMAGE_CREATE("qcow2-backing-raw-slice", "raw-slice");
    TEST_IMAGE_CREATE("qcow2-backing-qcow2-slice", "qcow2-slice");

    TEST_IMAGE_CREATE("network-gluster-qcow2", NULL);
    TEST_IMAGE_CREATE("network-rbd-qcow2", NULL);
    TEST_IMAGE_CREATE("network-ssh-qcow2", NULL);
    TEST_IMAGE_CREATE("network-sheepdog-qcow2", NULL);

#define TEST_BITMAP_DETECT(testname) \
    do { \
        if (virTestRun("bitmap detect " testname, \
                       testQemuDetectBitmaps, testname) < 0) \
            ret = -1; \
    } while (0)

    TEST_BITMAP_DETECT("basic");
    TEST_BITMAP_DETECT("synthetic");
    TEST_BITMAP_DETECT("snapshots");
    TEST_BITMAP_DETECT("snapshots-synthetic-checkpoint");
    TEST_BITMAP_DETECT("snapshots-synthetic-broken");

#define TEST_BACKUP_BITMAP_CALCULATE(testname, source, incrbackup, named) \
    do { \
        backupbitmapcalcdata.name = testname; \
        backupbitmapcalcdata.chain = source; \
        backupbitmapcalcdata.incremental = incrbackup; \
        backupbitmapcalcdata.nodedatafile = named; \
        if (virTestRun("incremental backup bitmap " testname, \
                       testQemuBackupIncrementalBitmapCalculate, \
                       &backupbitmapcalcdata) < 0) \
            ret = -1; \
    } while (0)

    TEST_BACKUP_BITMAP_CALCULATE("basic-flat", bitmapSourceChain, "current", "basic");
    TEST_BACKUP_BITMAP_CALCULATE("basic-intermediate", bitmapSourceChain, "d", "basic");
    TEST_BACKUP_BITMAP_CALCULATE("basic-deep", bitmapSourceChain, "a", "basic");

    TEST_BACKUP_BITMAP_CALCULATE("snapshot-flat", bitmapSourceChain, "current", "snapshots");
    TEST_BACKUP_BITMAP_CALCULATE("snapshot-intermediate", bitmapSourceChain, "d", "snapshots");
    TEST_BACKUP_BITMAP_CALCULATE("snapshot-deep", bitmapSourceChain, "a", "snapshots");

#define TEST_CHECKPOINT_DELETE_MERGE(testname, delbmp, parbmp, named) \
    do { \
        checkpointdeletedata.name = testname; \
        checkpointdeletedata.chain = bitmapSourceChain; \
        checkpointdeletedata.deletebitmap = delbmp; \
        checkpointdeletedata.parentbitmap = parbmp; \
        checkpointdeletedata.nodedatafile = named; \
        if (virTestRun("checkpoint delete " testname, \
                       testQemuCheckpointDeleteMerge, &checkpointdeletedata) < 0) \
        ret = -1; \
    } while (0)

    TEST_CHECKPOINT_DELETE_MERGE("basic-noparent", "a", NULL, "basic");
    TEST_CHECKPOINT_DELETE_MERGE("basic-intermediate1", "b", "a", "basic");
    TEST_CHECKPOINT_DELETE_MERGE("basic-intermediate2", "c", "b", "basic");
    TEST_CHECKPOINT_DELETE_MERGE("basic-intermediate3", "d", "c", "basic");
    TEST_CHECKPOINT_DELETE_MERGE("basic-current", "current", "d", "basic");

    TEST_CHECKPOINT_DELETE_MERGE("snapshots-noparent", "a", NULL, "snapshots");
    TEST_CHECKPOINT_DELETE_MERGE("snapshots-intermediate1", "b", "a", "snapshots");
    TEST_CHECKPOINT_DELETE_MERGE("snapshots-intermediate2", "c", "b", "snapshots");
    TEST_CHECKPOINT_DELETE_MERGE("snapshots-intermediate3", "d", "c", "snapshots");
    TEST_CHECKPOINT_DELETE_MERGE("snapshots-current", "current", "d", "snapshots");

    TEST_CHECKPOINT_DELETE_MERGE("snapshots-synthetic-checkpoint-noparent", "a", NULL, "snapshots-synthetic-checkpoint");
    TEST_CHECKPOINT_DELETE_MERGE("snapshots-synthetic-checkpoint-intermediate1", "b", "a", "snapshots-synthetic-checkpoint");
    TEST_CHECKPOINT_DELETE_MERGE("snapshots-synthetic-checkpoint-intermediate2", "c", "b", "snapshots-synthetic-checkpoint");
    TEST_CHECKPOINT_DELETE_MERGE("snapshots-synthetic-checkpoint-intermediate3", "d", "c", "snapshots-synthetic-checkpoint");
    TEST_CHECKPOINT_DELETE_MERGE("snapshots-synthetic-checkpoint-current", "current", "d", "snapshots-synthetic-checkpoint");

#define TEST_BITMAP_VALIDATE(testname, bitmap, rc) \
    do { \
        blockbitmapvalidatedata.name = testname; \
        blockbitmapvalidatedata.chain = bitmapSourceChain; \
        blockbitmapvalidatedata.bitmapname = bitmap; \
        blockbitmapvalidatedata.expect = rc; \
        if (virTestRun("bitmap validate " testname " " bitmap, \
                       testQemuBlockBitmapValidate, \
                       &blockbitmapvalidatedata) < 0) \
            ret = -1; \
    } while (0)

    TEST_BITMAP_VALIDATE("basic", "a", true);
    TEST_BITMAP_VALIDATE("basic", "b", true);
    TEST_BITMAP_VALIDATE("basic", "c", true);
    TEST_BITMAP_VALIDATE("basic", "d", true);
    TEST_BITMAP_VALIDATE("basic", "current", true);

    TEST_BITMAP_VALIDATE("snapshots", "a", true);
    TEST_BITMAP_VALIDATE("snapshots", "b", true);
    TEST_BITMAP_VALIDATE("snapshots", "c", true);
    TEST_BITMAP_VALIDATE("snapshots", "d", true);
    TEST_BITMAP_VALIDATE("snapshots", "current", true);

    TEST_BITMAP_VALIDATE("synthetic", "a", false);
    TEST_BITMAP_VALIDATE("synthetic", "b", true);
    TEST_BITMAP_VALIDATE("synthetic", "c", true);
    TEST_BITMAP_VALIDATE("synthetic", "d", true);
    TEST_BITMAP_VALIDATE("synthetic", "current", true);

    TEST_BITMAP_VALIDATE("snapshots-synthetic-checkpoint", "a", true);
    TEST_BITMAP_VALIDATE("snapshots-synthetic-checkpoint", "b", true);
    TEST_BITMAP_VALIDATE("snapshots-synthetic-checkpoint", "c", true);
    TEST_BITMAP_VALIDATE("snapshots-synthetic-checkpoint", "d", true);
    TEST_BITMAP_VALIDATE("snapshots-synthetic-checkpoint", "current", true);

    TEST_BITMAP_VALIDATE("snapshots-synthetic-broken", "a", false);
    TEST_BITMAP_VALIDATE("snapshots-synthetic-broken", "b", true);
    TEST_BITMAP_VALIDATE("snapshots-synthetic-broken", "c", true);
    TEST_BITMAP_VALIDATE("snapshots-synthetic-broken", "d", false);
    TEST_BITMAP_VALIDATE("snapshots-synthetic-broken", "current", true);

#define TEST_BITMAP_BLOCKCOPY(testname, shllw, ndf) \
    do { \
        blockbitmapblockcopydata.name = testname; \
        blockbitmapblockcopydata.shallow = shllw; \
        blockbitmapblockcopydata.nodedatafile = ndf; \
        blockbitmapblockcopydata.chain = bitmapSourceChain;\
        if (virTestRun("bitmap block copy " testname, \
                       testQemuBlockBitmapBlockcopy, \
                       &blockbitmapblockcopydata) < 0) \
            ret = -1; \
    } while (0)

    TEST_BITMAP_BLOCKCOPY("basic-shallow", true, "basic");
    TEST_BITMAP_BLOCKCOPY("basic-deep", false, "basic");

    TEST_BITMAP_BLOCKCOPY("snapshots-shallow", true, "snapshots");
    TEST_BITMAP_BLOCKCOPY("snapshots-deep", false, "snapshots");


#define TEST_BITMAP_BLOCKCOMMIT(testname, topimg, baseimg, ndf) \
    do {\
        blockbitmapblockcommitdata.name = testname; \
        blockbitmapblockcommitdata.top = testQemuBitmapGetFakeChainEntry(bitmapSourceChain, topimg); \
        blockbitmapblockcommitdata.base = testQemuBitmapGetFakeChainEntry(bitmapSourceChain, baseimg); \
        blockbitmapblockcommitdata.nodedatafile = ndf; \
        if (virTestRun("bitmap block commit " testname, \
                       testQemuBlockBitmapBlockcommit, \
                       &blockbitmapblockcommitdata) < 0) \
        ret = -1; \
    } while (0)

    TEST_BITMAP_BLOCKCOMMIT("basic-1-2", 1, 2, "basic");
    TEST_BITMAP_BLOCKCOMMIT("basic-1-3", 1, 3, "basic");
    TEST_BITMAP_BLOCKCOMMIT("basic-2-3", 2, 3, "basic");

    TEST_BITMAP_BLOCKCOMMIT("snapshots-1-2", 1, 2, "snapshots");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-1-3", 1, 3, "snapshots");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-1-4", 1, 4, "snapshots");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-1-5", 1, 5, "snapshots");

    TEST_BITMAP_BLOCKCOMMIT("snapshots-2-3", 2, 3, "snapshots");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-2-4", 2, 4, "snapshots");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-2-5", 2, 5, "snapshots");

    TEST_BITMAP_BLOCKCOMMIT("snapshots-3-4", 3, 4, "snapshots");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-3-5", 3, 5, "snapshots");

    TEST_BITMAP_BLOCKCOMMIT("snapshots-4-5", 4, 5, "snapshots");

    TEST_BITMAP_BLOCKCOMMIT("snapshots-synthetic-broken-1-2", 1, 2, "snapshots-synthetic-broken");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-synthetic-broken-1-3", 1, 3, "snapshots-synthetic-broken");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-synthetic-broken-1-4", 1, 4, "snapshots-synthetic-broken");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-synthetic-broken-1-5", 1, 5, "snapshots-synthetic-broken");

    TEST_BITMAP_BLOCKCOMMIT("snapshots-synthetic-broken-2-3", 2, 3, "snapshots-synthetic-broken");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-synthetic-broken-2-4", 2, 4, "snapshots-synthetic-broken");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-synthetic-broken-2-5", 2, 5, "snapshots-synthetic-broken");

    TEST_BITMAP_BLOCKCOMMIT("snapshots-synthetic-broken-3-4", 3, 4, "snapshots-synthetic-broken");
    TEST_BITMAP_BLOCKCOMMIT("snapshots-synthetic-broken-3-5", 3, 5, "snapshots-synthetic-broken");

    TEST_BITMAP_BLOCKCOMMIT("snapshots-synthetic-broken-4-5", 4, 5, "snapshots-synthetic-broken");

 cleanup:
    qemuTestDriverFree(&driver);
    VIR_FREE(capslatest_x86_64);
    virObjectUnref(caps_x86_64);

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN_PRELOAD(mymain, VIR_TEST_MOCK("virdeterministichash"))
