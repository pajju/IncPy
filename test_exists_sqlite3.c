// canary file to see if sqlite3.h has been installed on this machine
// (if this compile fails, then sqlite3.h likely doesn't exist)
#include "sqlite3.h"

int main() {
  return 0;
}
