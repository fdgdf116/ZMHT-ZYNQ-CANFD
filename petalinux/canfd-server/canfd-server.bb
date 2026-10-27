SUMMARY = "ZYNQ CANFD SocketCAN TCP service"
SECTION = "PETALINUX/apps"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://Makefile \
    file://canfd-server.init \
    file://canfd-src.tar.gz \
"

S = "${WORKDIR}"

inherit update-rc.d

INITSCRIPT_NAME = "canfd-server"
INITSCRIPT_PARAMS = "start 99 5 . stop 01 0 6 ."

do_compile() {
    oe_runmake
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/canfd_server ${D}${bindir}/canfd_server

    install -d ${D}${sysconfdir}/init.d
    install -m 0755 ${S}/canfd-server.init ${D}${sysconfdir}/init.d/canfd-server
}

FILES_${PN} = "${bindir}/canfd_server ${sysconfdir}/init.d/canfd-server"
