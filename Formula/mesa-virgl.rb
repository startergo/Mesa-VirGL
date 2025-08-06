class MesaVirgl < Formula
  desc "Mesa 3D Graphics Library with VirGL (Virgil3D) support for macOS"
  homepage "https://mesa3d.org"
  url "https://archive.mesa3d.org/mesa-24.3.0.tar.xz"
  version "24.3.0-virgl"
  sha256 "8b8ccaf25b87e8ad47e8230e7c2b1b96583ac20b518e969b5d16dc6ffccac0a8"
  license "MIT"
  revision 1

  head "https://github.com/startergo/Mesa-VirGL.git", branch: "main"

  # Build dependencies
  depends_on "meson" => :build
  depends_on "ninja" => :build
  depends_on "pkg-config" => :build
  depends_on "python@3.12" => :build
  depends_on "bison" => :build
  depends_on "flex" => :build

  # Core Mesa dependencies
  depends_on "libx11"
  depends_on "libxext"
  depends_on "libxdamage"
  depends_on "libxfixes"
  depends_on "libxrandr"
  depends_on "libxxf86vm"
  depends_on "libxcb"
  depends_on "libxshmfence"
  depends_on "expat"
  depends_on "gettext"
  depends_on "zlib"
  depends_on "zstd"

  # Graphics and rendering dependencies
  depends_on "libdrm"
  depends_on "wayland" => :optional
  depends_on "wayland-protocols" => :optional

  # VirGL specific dependencies
  depends_on "libepoxy"
  depends_on "libpng"

  # Optional dependencies for full functionality
  depends_on "llvm" => :optional
  depends_on "libva" => :optional
  depends_on "libvdpau" => :optional

  def install
    # Set up build environment
    ENV["PKG_CONFIG_PATH"] = "#{HOMEBREW_PREFIX}/lib/pkgconfig"
    
    # Add X11 paths for macOS
    ENV.append "CPPFLAGS", "-I/opt/X11/include"
    ENV.append "LDFLAGS", "-L/opt/X11/lib"
    
    # Configure Mesa build arguments - using the exact configuration from README.rst
    meson_args = [
      "--prefix=#{prefix}",
      "--buildtype=release",
      "-Dgallium-va=disabled",
      "-Dgallium-drivers=virgl",
      "-Dvulkan-drivers=",
      "-Dglx=xlib",
      "-Dplatforms=x11"
    ]

    # Create build directory and configure
    mkdir "build" do
      system "meson", "setup", "..", *meson_args
      system "ninja"
      system "ninja", "install"
    end

    # Create version info
    (prefix/"VERSION").write("#{version}-#{revision}")
    
    # Create VirGL info file
    create_virgl_info_file
  end

  def create_virgl_info_file
    info_content = <<~INFO
      Mesa VirGL (Virgil3D) Build Information
      ======================================
      
      Version: #{version}
      Build Date: #{Time.now.strftime("%Y-%m-%d %H:%M:%S")}
      
      Configuration:
      - Gallium Drivers: virgl
      - Vulkan Drivers: disabled
      - GLX: xlib
      - Platforms: x11
      - OpenGL: enabled
      - OpenGL ES 2.0: enabled
      - EGL: enabled
      
      VirGL Features:
      - Hardware accelerated OpenGL over VirtIO
      - Guest-to-host GPU virtualization
      - Compatible with QEMU VirGL backend
      
      Usage:
      This build is optimized for VirGL (Virgil3D) virtualized graphics.
      Use with QEMU's -device virtio-vga-gl option for hardware acceleration.
      
      Library Location: #{lib}
      Headers Location: #{include}
      
      For more information: https://mesa3d.org
    INFO
    
    (prefix/"VIRGL_INFO.txt").write(info_content)
  end

  def post_install
    ohai "Mesa VirGL installation completed!"
    ohai "VirGL libraries installed to: #{lib}"
    ohai "Headers installed to: #{include}"
    ohai ""
    ohai "To use with QEMU VirGL:"
    ohai "  1. Install QEMU with VirGL support"
    ohai "  2. Use: -device virtio-vga-gl -display sdl,gl=on"
    ohai "  3. Ensure XQuartz is installed for X11 support"
    ohai ""
    ohai "Build information saved to: #{prefix}/VIRGL_INFO.txt"
  end

  test do
    # Test that basic Mesa libraries are available
    (testpath/"test.c").write <<~C_CODE
      #include <GL/gl.h>
      #include <stdio.h>
      
      int main() {
          printf("Mesa VirGL test\\n");
          printf("GL_VERSION: %s\\n", glGetString(GL_VERSION));
          printf("GL_VENDOR: %s\\n", glGetString(GL_VENDOR));
          printf("GL_RENDERER: %s\\n", glGetString(GL_RENDERER));
          return 0;
      }
    C_CODE

    # Try to compile the test (this will test that headers and libraries are properly installed)
    system ENV.cc, "test.c", "-I#{include}", "-L#{lib}", "-lGL", "-o", "test"
    
    # Test that VirGL driver library exists
    assert_predicate lib/"dri/virtio_gpu_dri.so", :exist?, "VirGL driver not found"
    
    # Test that EGL library exists
    assert_predicate lib/"libEGL.dylib", :exist?, "EGL library not found"
    
    # Test that OpenGL ES library exists
    assert_predicate lib/"libGLESv2.dylib", :exist?, "OpenGL ES library not found"
    
    # Verify version information
    version_info = (prefix/"VERSION").read.strip
    assert_match(/#{version}/, version_info, "Version file not correct")
  end
end
