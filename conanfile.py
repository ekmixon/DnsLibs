from conans import ConanFile, CMake


class DnsLibsConan(ConanFile):
    name = "dns-libs"
    version = "777"  # use the `commit_hash` option to select the desired library version
    license = "Apache-2.0"
    author = "AdguardTeam"
    url = "https://github.com/AdguardTeam/DnsLibs"
    description = "A DNS proxy library that supports all existing DNS protocols"
    topics = ("dns", "proxy", "security", "adblock", "privacy")
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "commit_hash": "ANY",
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "commit_hash": None,  # None means `master`
    }
    generators = "cmake"

    def requirements(self):
        self.requires("spdlog/1.8.0")
        self.requires("magic_enum/0.7.3")
        self.requires("pcre2/10.37@AdguardTeam/NativeLibsCommon")
        self.requires("libsodium/1.0.18@AdguardTeam/NativeLibsCommon")
        self.requires("libcurl/7.78.0@AdguardTeam/NativeLibsCommon")
        self.requires("libevent/2.1.11@AdguardTeam/NativeLibsCommon")
        self.requires("libuv/1.41.0@AdguardTeam/NativeLibsCommon")
        self.requires("klib/2021-04-06@AdguardTeam/NativeLibsCommon")
        self.requires("ldns/2021-03-29@AdguardTeam/NativeLibsCommon")
        self.requires("ngtcp2/2021-05-13@AdguardTeam/NativeLibsCommon")

    def build_requirements(self):
        self.build_requires("gtest/1.11.0")

    def configure(self):
        self.options["spdlog"].no_exceptions = True
        self.options["gtest"].build_gmock = False

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def source(self):
        self.run("git clone https://github.com/AdguardTeam/DnsLibs.git source_subfolder")

        if self.options.commit_hash:
            self.run("cd source_subfolder && git checkout %s" % self.options.commit_hash)

    def build(self):
        cmake = CMake(self)
        cmake.configure(source_folder="source_subfolder/proxy")
        cmake.build(target="dnsproxy")

    def package(self):
        MODULES = [
            "common",
            "dnscrypt",
            "dnsfilter",
            "dnsstamp",
            "net",
            "proxy",
            "upstream",
        ]

        for m in MODULES:
            self.copy("*.h", dst="include", src="source_subfolder/%s/include" % m, keep_path=False)

        self.copy("*.lib", dst="lib", keep_path=False)
        self.copy("*.dll", dst="bin", keep_path=False)
        self.copy("*.so", dst="lib", keep_path=False)
        self.copy("*.dylib", dst="lib", keep_path=False)
        self.copy("*.a", dst="lib", keep_path=False)

    def package_info(self):
        self.cpp_info.name = "dns-libs"
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.libs = [
            "dnsproxy",
            "dnsfilter",
            "upstream",
            "dnscrypt",
            "dnsstamp",
            "dnslibs_net",
            "dnslibs_common",
        ]
        self.cpp_info.libdirs = ['lib']
        self.cpp_info.requires = [
            "spdlog::spdlog",
            "magic_enum::magic_enum",
            "pcre2::pcre2",
            "libsodium::libsodium",
            "libcurl::libcurl",
            "libevent::libevent",
            "libuv::libuv",
            "klib::klib",
            "ldns::ldns",
            "ngtcp2::ngtcp2",
        ]
