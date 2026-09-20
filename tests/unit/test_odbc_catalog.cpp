#include "odbc/schema_catalog.hpp"

#include "check.hpp"

using namespace uniorm;

void test_odbc_catalog_name_matches() {
  // Asked for as a name: the driver's own spelling of the row decides.
  CHECK(odbc::catalog_name_matches("user_orders", "user_orders"));
  // Case is not a difference here; how a name is emitted is a policy, not a
  // fact about which rows a read was about.
  CHECK(odbc::catalog_name_matches("USER_ORDERS", "user_orders"));
  // A name widened by the pattern rules of a catalog call: a different table.
  CHECK(!odbc::catalog_name_matches("user_orders", "userxorders"));
  CHECK(!odbc::catalog_name_matches("a%", "abc"));
  CHECK(!odbc::catalog_name_matches("orders", "order"));
  CHECK(odbc::catalog_name_matches("a%", "a%"));
  // Nothing asked, or nothing reported by the driver, leaves the row alone.
  CHECK(odbc::catalog_name_matches("", "anything"));
  CHECK(odbc::catalog_name_matches("user_orders", ""));
  CHECK(odbc::catalog_name_matches("", ""));
  // Folding stops at ASCII, so a non-ASCII name cannot fold onto another:
  // both of these are 7 bytes.
  CHECK(!odbc::catalog_name_matches("strasse", "stra\xc3\x9f" "e"));
  CHECK(odbc::catalog_name_matches("stra\xc3\x9f" "e", "stra\xc3\x9f" "e"));
}
