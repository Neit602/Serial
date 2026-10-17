#pragma once

// Kermit's packet layer (PRD F19), exposed for the tests; the engines are in
// kermit.cpp.  Long packets and attribute packets are supported; sliding
// windows and streaming are not (DECISIONS D55).

#include <QByteArray>
#include <QByteArrayView>

#include <optional>

namespace portside::transfer::kermit {

inline char tochar(int value) { return char(value + 32); }
inline int unchar(char c) { return int(quint8(c)) - 32; }
inline char ctl(char c) { return char(c ^ 64); }

// What one side declares in its Send-Init (S) packet or the ACK to it.
struct InitParams
{
    int maxLength = 94;      // longest short packet this side can receive
    int timeout = 10;        // seconds the other side should wait for it
    int padCount = 0;
    char padChar = 0;
    char eol = '\r';
    char ctlPrefix = '#';
    char binPrefix = 'Y';    // 'Y': will do 8th-bit prefixing if asked
    int checkType = 3;
    char repeatPrefix = '~';
    bool longPackets = true;
    bool attributes = true;
    int longLength = 4096;   // longest long packet this side can receive
};

QByteArray encodeInit(const InitParams &params);
InitParams decodeInit(QByteArrayView data);

// The settings both sides end up using.
struct Agreed
{
    int checkType = 1;
    // Longest packet we may send, in the units the length fields count.
    int sendLength = 80;
    bool longPackets = false;
    bool attributes = false;
    char ctlPrefixOut = '#';  // the prefix we use
    char ctlPrefixIn = '#';   // the prefix the other side uses
    bool binQuoting = false;
    char binPrefix = '&';
    bool repeating = false;
    char repeatPrefix = '~';
    char eol = '\r';
    int padCount = 0;
    char padChar = 0;
    int timeoutMs = 10000;

    // Room for data in one packet of ours.
    int dataCapacity() const;
};

Agreed agree(const InitParams &ours, const InitParams &theirs);

// The block check over `body` (from the LEN field to the end of the data).
QByteArray blockCheck(QByteArrayView body, int type);

QByteArray encodePacket(char type, int sequence, QByteArrayView data, int checkType,
                        const Agreed &agreed);

// Encode as much of `input` as fits in `capacity` characters; `consumed`
// says how many input bytes that took.
QByteArray encodeData(QByteArrayView input, int capacity, const Agreed &agreed,
                      qsizetype *consumed);
// Undo encodeData.  Empty when the field is malformed.
std::optional<QByteArray> decodeData(QByteArrayView data, const Agreed &agreed);

} // namespace portside::transfer::kermit
