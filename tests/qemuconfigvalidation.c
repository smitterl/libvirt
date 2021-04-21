#include <stdio.h>
#include <string.h>

#include "qemu/qemu_conf.c"


static int
tls_path_not_existing(char *config_param)
{
        bool privileged = false;
        char * root = NULL;
        virQEMUDriverConfig *cfg;
        cfg = virQEMUDriverConfigNew(privileged, root);
        virQEMUDriverConfigSetDefaults(cfg);

        if(strcmp(config_param, "default_tls_x509_cert_dir") == 0){
                cfg.defaultTLSx509certdir = "/not/existing/path";
        }
        if(strcmp(config_param, "vnc_tls_x509_cert_dir") == 0){
                cfg.vncTLSx509certdir = "/not/existing/path";
        }
        if(strcmp(config_param, "spice_tls_x509_cert_dir") == 0){
                cfg.spiceTLSx509certdir = "/not/existing/path";
        }
        if(strcmp(config_param, "chardev_tls_x509_cert_dir") == 0){
                cfg.chardevTLSx509certdir = "/not/existing/path";
        }
        if(strcmp(config_param, "vxhs_tls_x509_cert_dir") == 0){
                cfg.vxhsTLSx509certdir = "/not/existing/path";
        }
        if(strcmp(config_param, "nbd_tls_x509_cert_dir") == 0){
                cfg.nbdTLSx509certdir = "/not/existing/path";
        }
        if(strcmp(config_param, "migrate_tls_x509_cert_dir") == 0){
                cfg.migrateTLSx509certdir = "/not/existing/path";
        }
        if(strcmp(config_param, "backup_tls_x509_cert_dir") == 0){
                cfg.backupTLSx509certdir = "/not/existing/path";
        }

        if(virQEMUDriverConfigValidate(cfg) != -1) { return -1; }

        char * log;
        log = virTestLogContentAndReset();
        if(!strstr(log, config_param)){
                printf("config parameter name is missing in error message");
                printf("got: %s", log);
        }
}

static int testsuite(void)
{
        int ret = 0;

#define DO_TEST(NAME)\
        if(virTestRun("QEMU tls config validation test " #NAME,\
                NAME, NULL) < 0)\
                ret = -1

        DO_TEST(tls_path_not_existing("default_tls_x509_cert_dir"));
        DO_TEST(tls_path_not_existing("vnc_tls_x509_cert_dir"));
        DO_TEST(tls_path_not_existing("spice_tls_x509_cert_dir"));
        DO_TEST(tls_path_not_existing("chardev_tls_x509_cert_dir"));
        DO_TEST(tls_path_not_existing("vxhs_tls_x509_cert_dir"));
        DO_TEST(tls_path_not_existing("nbd_tls_x509_cert_dir"));
        DO_TEST(tls_path_not_existing("migrate_tls_x509_cert_dir"));
        DO_TEST(tls_path_not_existing("backup_tls_x509_cert_dir"));


        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN(testsuite);
