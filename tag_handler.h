#include <MFRC522.h>
#include "config.h"

const char * uid_print(const MFRC522::Uid uid);
void uid_copy(MFRC522::Uid &a, const MFRC522::Uid b);
bool uid_compare(const MFRC522::Uid a, const MFRC522::Uid b);
bool uid_auth(const MFRC522::Uid uid);
void uid_zero(MFRC522::Uid &uid);
bool uid_visible(MFRC522 &reader, const MFRC522::Uid master);
bool uid_isset(const MFRC522::Uid uid);
bool uid_fetch(MFRC522 &reader, MFRC522::Uid &uid, int count);
bool uid_valid(MFRC522::Uid &uid);

