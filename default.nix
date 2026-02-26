{
  lib,
  stdenv,
  kernel,
  kmod,
}:

stdenv.mkDerivation {
  pname = "tbs5930";
  version = "0-unstable-${kernel.version}";

  src = lib.fileset.toSource {
    root = ./.;
    fileset = ./drivers/media;
  };

  nativeBuildInputs = [ kmod ] ++ kernel.moduleBuildDependencies;
  hardeningDisable = [ "pic" ];

  KERNEL_DIR = "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build";

  postPatch = ''
    # Fix include paths: $(srctree)/drivers/media/* -> $(M)/* in M= builds
    find drivers/media -name Makefile -exec \
      sed -i 's|$(srctree)/drivers/media/|$(M)/|g' {} +

    # Replace top-level Makefile to only build needed subdirs
    # Skip dvb-core/ (use kernel's built-in dvb-core)
    cat > drivers/media/Makefile << 'EOF'
    obj-$(CONFIG_DVB_CORE) += dvb-frontends/
    obj-y += usb/
    EOF

    # Only build dvb-usb-v2/ under usb/
    cat > drivers/media/usb/Makefile << 'EOF'
    obj-y += dvb-usb-v2/
    EOF

    # Only build TBS5930 under usb/dvb-usb-v2/ (dvb_usb_v2 framework comes from kernel)
    cat > drivers/media/usb/dvb-usb-v2/Makefile << 'EOF'
    dvb-usb-tbs5930-objs := tbs5930.o
    obj-$(CONFIG_DVB_USB_TBS5930) += dvb-usb-tbs5930.o
    ccflags-y += -I$(M)/dvb-frontends/
    ccflags-y += -I$(M)/usb/dvb-usb-v2/
    EOF

    # Only build m88rs6060 under dvb-frontends/
    cat > drivers/media/dvb-frontends/Makefile << 'EOF'
    obj-$(CONFIG_DVB_M88RS6060) += m88rs6060.o
    EOF

    # Patch m88rs6060.c for upstream kernel 6.19 compatibility
    pushd drivers/media/dvb-frontends

    # Add compat defines for missing FEC rates
    sed -i '/#include "m88rs6060_priv.h"/a \
    #ifndef FEC_29_45\n#define FEC_29_45 FEC_AUTO\n#endif\
    \n#ifndef FEC_31_45\n#define FEC_31_45 FEC_AUTO\n#endif' m88rs6060.c

    # Remove TBS-specific ops + dead code functions that reference
    # TBS-custom structs (ecp3_info, eeprom_info) not in upstream
    sed -i '/\.spi_read = /d; /\.spi_write = /d; /\.eeprom_read = /d; /\.eeprom_write = /d' m88rs6060.c
    sed -i '/^static void m88rs6060_spi_read/,/^}$/d' m88rs6060.c
    sed -i '/^static void m88rs6060_spi_write/,/^}$/d' m88rs6060.c
    sed -i '/^static void m88rs6060_eeprom_read/,/^}$/d' m88rs6060.c
    sed -i '/^static void m88rs6060_eeprom_write/,/^}$/d' m88rs6060.c

    popd
  '';

  MAKE_ARGS = lib.concatStringsSep " " [
    "CONFIG_MEDIA_SUPPORT=m"
    "CONFIG_DVB_CORE=m"
    "CONFIG_DVB_NET=y"
    "CONFIG_DVB_USB_TBS5930=m"
    "CONFIG_DVB_M88RS6060=m"
  ];

  buildPhase = ''
    runHook preBuild
    make -C $KERNEL_DIR M=$(pwd)/drivers/media $MAKE_ARGS modules
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    make -C $KERNEL_DIR M=$(pwd)/drivers/media $MAKE_ARGS \
      INSTALL_MOD_PATH=$out modules_install
    runHook postInstall
  '';

  meta = {
    description = "TBS DVB media drivers (out-of-tree)";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}
