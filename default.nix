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

  MAKE_ARGS = lib.concatStringsSep " " [
    "CONFIG_MEDIA_SUPPORT=m"
    "CONFIG_DVB_CORE=m"
    "CONFIG_DVB_NET=y"
    "CONFIG_DVB_USB_TBS5930=m"
    "CONFIG_DVB_M88RS6060=m"
    "KCFLAGS=-Wall -Wextra -Werror"
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
