#include "camera/camera.hpp"
#include <iostream>
namespace nss::camera {
Json qualify(const std::string &scope);
}
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      return 2;
    std::cout << nss::camera::qualify(argv[1]).dump() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
