#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Forward declaration for progress callback
typedef struct DataWin DataWin;

typedef struct {
    bool parseGen8;
    bool parseOptn;
    bool parseLang;
    bool parseExtn;
    bool parseSond;
    bool parseAgrp;
    bool parseSprt;
    bool parseBgnd;
    bool parsePath;
    bool parseScpt;
    bool parseGlob;
    bool parseShdr;
    bool parseFont;
    bool parseTmln;
    bool parseObjt;
    bool parseRoom;
    bool parseTpag;
    bool parseCode;
    bool parseVari;
    bool parseFunc;
    bool parseStrg;
    bool parseTxtr;
    bool parseAudo;
    // If true, precise masks will be skipped when the sprite does not have a precise state set
    bool skipLoadingPreciseMasksForNonPreciseSprites;

    // Optional progress callback, called before each chunk is parsed.
    // chunkName: 4-character chunk name (e.g. "GEN8", "SPRT")
    // chunkIndex: 0-based index of the current chunk being parsed
    // totalChunks: total number of chunks in the file
    // dataWin: the DataWin being populated (earlier chunks may already be parsed)
    // userData: user-provided pointer passed through from the options
    void (*progressCallback)(const char* chunkName, int chunkIndex, int totalChunks, DataWin* dataWin, void* userData);
    void* progressCallbackUserData;
} DataWinParserOptions;

// ===[ GEN8 - General Info ]===
typedef struct {
    uint8_t isDebuggerDisabled;
    uint8_t bytecodeVersion;
    const char* fileName;
    const char* config;
    uint32_t lastObj;
    uint32_t lastTile;
    uint32_t gameID;
    uint8_t directPlayGuid[16];
    const char* name;
    uint32_t major;
    uint32_t minor;
    uint32_t release;
    uint32_t build;
    uint32_t defaultWindowWidth;
    uint32_t defaultWindowHeight;
    uint32_t info;
    uint32_t licenseCRC32;
    uint8_t licenseMD5[16];
    uint64_t timestamp;
    const char* displayName;
    uint64_t activeTargets;
    uint64_t functionClassifications;
    int32_t steamAppID;
    uint32_t debuggerPort;
    uint32_t roomOrderCount;
    int32_t* roomOrder;
} Gen8;

// ===[ OPTN - Options ]===
typedef struct {
    const char* name;
    const char* value;
} OptnConstant;

typedef struct {
    uint64_t info;
    int32_t scale;
    uint32_t windowColor;
    uint32_t colorDepth;
    uint32_t resolution;
    uint32_t frequency;
    uint32_t vertexSync;
    uint32_t priority;
    uint32_t backImage;
    uint32_t frontImage;
    uint32_t loadImage;
    uint32_t loadAlpha;
    uint32_t constantCount;
    OptnConstant* constants;
} Optn;

// ===[ LANG - Languages ]===
typedef struct {
    const char* name;
    const char* region;
    uint32_t entryCount;
    const char** entries;
} Language;

typedef struct {
    uint32_t unknown1;
    uint32_t languageCount;
    uint32_t entryCount;
    const char** entryIds;
    Language* languages;
} Lang;

// ===[ EXTN - Extensions ]===
typedef struct {
    const char* name;
    uint32_t id;
    uint32_t kind;
    uint32_t retType;
    const char* extName;
    uint32_t argumentCount;
    uint32_t* arguments;
} ExtensionFunction;

typedef struct {
    const char* filename;
    const char* cleanupScript;
    const char* initScript;
    uint32_t kind;
    uint32_t functionCount;
    ExtensionFunction* functions;
} ExtensionFile;

typedef struct {
    const char* folderName;
    const char* name;
    const char* className;
    uint32_t fileCount;
    ExtensionFile* files;
} Extension;

typedef struct {
    uint32_t count;
    Extension* extensions;
} Extn;

// ===[ SOND - Sounds ]===
typedef struct {
    const char* name;
    uint32_t flags;
    const char* type;
    const char* file;
    uint32_t effects;
    float volume;
    float pitch;
    int32_t audioGroup;
    int32_t audioFile;
} Sound;

typedef struct {
    uint32_t count;
    Sound* sounds;
} Sond;

// ===[ AGRP - Audio Groups ]===
typedef struct {
    const char* name;
} AudioGroup;

typedef struct {
    uint32_t count;
    AudioGroup* audioGroups;
} Agrp;

// ===[ SPRT - Sprites ]===
typedef struct {
    const char* name;
    uint32_t width;
    uint32_t height;
    int32_t marginLeft;
    int32_t marginRight;
    int32_t marginBottom;
    int32_t marginTop;
    bool transparent;
    bool smooth;
    bool preload;
    uint32_t bboxMode;
    uint32_t sepMasks;
    int32_t originX;
    int32_t originY;
    uint32_t textureCount;
    uint32_t* textureOffsets; // absolute file offsets to TexturePageItems
    uint32_t maskCount;       // number of collision masks (one per frame, or 0)
    uint8_t** masks;          // array of maskCount packed bit arrays (nullptr if none)
} Sprite;

typedef struct {
    uint32_t count;
    Sprite* sprites;
} Sprt;

// ===[ BGND - Backgrounds ]===
typedef struct {
    const char* name;
    bool transparent;
    bool smooth;
    bool preload;
    uint32_t textureOffset; // absolute file offset to TexturePageItem
} Background;

typedef struct {
    uint32_t count;
    Background* backgrounds;
} Bgnd;

// ===[ PATH - Paths ]===
typedef struct {
    float x;
    float y;
    float speed;
} PathPoint;

typedef struct {
    double x;
    double y;
    double speed;
    double l; // cumulative arc length from start
} InternalPathPoint;

typedef struct {
    double x;
    double y;
    double speed;
} PathPositionResult;

typedef struct {
    const char* name;
    bool isSmooth;
    bool isClosed;
    uint32_t precision;
    uint32_t pointCount;
    PathPoint* points;
    uint32_t internalPointCount;
    InternalPathPoint* internalPoints;
    double length; // total arc length
} GamePath;

typedef struct {
    uint32_t count;
    GamePath* paths;
} PathChunk;

// ===[ SCPT - Scripts ]===
typedef struct {
    const char* name;
    int32_t codeId;
} Script;

typedef struct {
    uint32_t count;
    Script* scripts;
} Scpt;

// ===[ GLOB - Global Init Scripts ]===
typedef struct {
    uint32_t count;
    int32_t* codeIds;
} Glob;

// ===[ SHDR - Shaders ]===
typedef struct {
    const char* name;
    uint32_t type;
    const char* glslES_Vertex;
    const char* glslES_Fragment;
    const char* glsl_Vertex;
    const char* glsl_Fragment;
    const char* hlsl9_Vertex;
    const char* hlsl9_Fragment;
    uint32_t hlsl11_VertexOffset;
    uint32_t hlsl11_PixelOffset;
    uint32_t vertexAttributeCount;
    const char** vertexAttributes;
    int32_t version;
    uint32_t pssl_VertexOffset;
    uint32_t pssl_VertexLen;
    uint32_t pssl_PixelOffset;
    uint32_t pssl_PixelLen;
    uint32_t cgVita_VertexOffset;
    uint32_t cgVita_VertexLen;
    uint32_t cgVita_PixelOffset;
    uint32_t cgVita_PixelLen;
    uint32_t cgPS3_VertexOffset;
    uint32_t cgPS3_VertexLen;
    uint32_t cgPS3_PixelOffset;
    uint32_t cgPS3_PixelLen;
} Shader;

typedef struct {
    uint32_t count;
    Shader* shaders;
} Shdr;

// ===[ FONT - Fonts ]===
typedef struct {
    int16_t character;
    int16_t shiftModifier;
} KerningPair;

typedef struct {
    uint16_t character;
    uint16_t sourceX;
    uint16_t sourceY;
    uint16_t sourceWidth;
    uint16_t sourceHeight;
    int16_t shift;
    int16_t offset;
    uint16_t kerningCount;
    KerningPair* kerning;
} FontGlyph;

typedef struct {
    const char* name;
    const char* displayName;
    uint32_t emSize;
    bool bold;
    bool italic;
    uint16_t rangeStart;
    uint8_t charset;
    uint8_t antiAliasing;
    uint32_t rangeEnd;
    uint32_t textureOffset; // absolute file offset to TexturePageItem
    float scaleX;
    float scaleY;
    uint32_t glyphCount;
    FontGlyph* glyphs;
} Font;

typedef struct {
    uint32_t count;
    Font* fonts;
} FontChunk;

// ===[ EventAction (shared by TMLN and OBJT) ]===
typedef struct {
    uint32_t libID;
    uint32_t id;
    uint32_t kind;
    bool useRelative;
    bool isQuestion;
    bool useApplyTo;
    uint32_t exeType;
    const char* actionName;
    int32_t codeId;
    uint32_t argumentCount;
    int32_t who;
    bool relative;
    bool isNot;
    uint32_t unknownAlwaysZero;
} EventAction;

// ===[ TMLN - Timelines ]===
typedef struct {
    uint32_t step;
    uint32_t actionCount;
    EventAction* actions;
} TimelineMoment;

typedef struct {
    const char* name;
    uint32_t momentCount;
    TimelineMoment* moments;
} Timeline;

typedef struct {
    uint32_t count;
    Timeline* timelines;
} Tmln;

// ===[ OBJT - Game Objects ]===
#define OBJT_EVENT_TYPE_COUNT 12

typedef struct {
    uint32_t eventSubtype;
    uint32_t actionCount;
    EventAction* actions;
} ObjectEvent;

typedef struct {
    uint32_t eventCount;
    ObjectEvent* events;
} ObjectEventList;

typedef struct {
    float x;
    float y;
} PhysicsVertex;

typedef struct {
    const char* name;
    int32_t spriteId;
    bool visible;
    bool solid;
    int32_t depth;
    bool persistent;
    int32_t parentId;
    int32_t textureMaskId;
    bool usesPhysics;
    bool isSensor;
    uint32_t collisionShape;
    float density;
    float restitution;
    uint32_t group;
    float linearDamping;
    float angularDamping;
    int32_t physicsVertexCount;
    float friction;
    bool awake;
    bool kinematic;
    PhysicsVertex* physicsVertices;
    ObjectEventList eventLists[OBJT_EVENT_TYPE_COUNT];
} GameObject;

typedef struct {
    uint32_t count;
    GameObject* objects;
} Objt;

// ===[ ROOM - Rooms ]===
typedef struct {
    bool enabled;
    bool foreground;
    int32_t backgroundDefinition;
    int32_t x;
    int32_t y;
    int32_t tileX;
    int32_t tileY;
    int32_t speedX;
    int32_t speedY;
    bool stretch;
} RoomBackground;

typedef struct {
    bool enabled;
    int32_t viewX;
    int32_t viewY;
    int32_t viewWidth;
    int32_t viewHeight;
    int32_t portX;
    int32_t portY;
    int32_t portWidth;
    int32_t portHeight;
    uint32_t borderX;
    uint32_t borderY;
    int32_t speedX;
    int32_t speedY;
    int32_t objectId;
} RoomView;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t objectDefinition;
    uint32_t instanceID;
    int32_t creationCode;
    float scaleX;
    float scaleY;
    uint32_t color;
    float rotation;
    int32_t preCreateCode;
} RoomGameObject;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t backgroundDefinition;
    int32_t sourceX;
    int32_t sourceY;
    uint32_t width;
    uint32_t height;
    int32_t tileDepth;
    uint32_t instanceID;
    float scaleX;
    float scaleY;
    uint32_t color;
} RoomTile;

typedef struct {
    const char* name;
    const char* caption;
    uint32_t width;
    uint32_t height;
    uint32_t speed;
    bool persistent;
    uint32_t backgroundColor;
    bool drawBackgroundColor;
    int32_t creationCodeId;
    uint32_t flags;
    bool world;
    uint32_t top;
    uint32_t left;
    uint32_t right;
    uint32_t bottom;
    float gravityX;
    float gravityY;
    float metersPerPixel;
    RoomBackground backgrounds[8];
    RoomView views[8];
    uint32_t gameObjectCount;
    RoomGameObject* gameObjects;
    uint32_t tileCount;
    RoomTile* tiles;
} Room;

typedef struct {
    uint32_t count;
    Room* rooms;
} RoomChunk;

// ===[ TPAG - Texture Page Items ]===
typedef struct {
    uint16_t sourceX;
    uint16_t sourceY;
    uint16_t sourceWidth;
    uint16_t sourceHeight;
    uint16_t targetX;
    uint16_t targetY;
    uint16_t targetWidth;
    uint16_t targetHeight;
    uint16_t boundingWidth;
    uint16_t boundingHeight;
    int16_t texturePageId;
} TexturePageItem;

typedef struct {
    uint32_t count;
    TexturePageItem* items;
} Tpag;

// ===[ CODE - Code Entries ]===
typedef struct {
    const char* name;
    uint32_t length;
    uint16_t localsCount;
    uint16_t argumentsCount;
    uint32_t bytecodeAbsoluteOffset;
    uint32_t offset;
} CodeEntry;

typedef struct {
    uint32_t count;
    CodeEntry* entries;
} Code;

// ===[ VARI - Variables ]===
typedef struct {
    const char* name;
    int32_t instanceType;
    int32_t varID;
    uint32_t occurrences;
    uint32_t firstAddress;
} Variable;

typedef struct {
    uint32_t varCount1;
    uint32_t varCount2;
    uint32_t maxLocalVarCount;
    uint32_t variableCount;
    Variable* variables;
} Vari;

// ===[ FUNC - Functions & Code Locals ]===
typedef struct {
    const char* name;
    uint32_t occurrences;
    uint32_t firstAddress;
} Function;

typedef struct {
    uint32_t index;
    const char* name;
} LocalVar;

typedef struct {
    const char* name;
    uint32_t localVarCount;
    LocalVar* locals;
} CodeLocals;

typedef struct {
    uint32_t functionCount;
    Function* functions;
    uint32_t codeLocalsCount;
    CodeLocals* codeLocals;
} Func;

// ===[ STRG - Strings ]===
typedef struct {
    uint32_t count;
    const char** strings; // pointers into strgBuffer
} Strg;

// ===[ TXTR - Embedded Textures ]===
typedef struct {
    uint32_t scaled;
    uint32_t blobOffset; // absolute file offset to PNG data
    uint32_t blobSize;   // computed size of blob data
    uint8_t* blobData;   // owned copy of PNG data
} Texture;

typedef struct {
    uint32_t count;
    Texture* textures;
} Txtr;

// ===[ AUDO - Embedded Audio ]===
typedef struct {
    uint32_t dataOffset; // absolute file offset to audio data
    uint32_t dataSize;   // length of audio data
    uint8_t* data;       // owned copy of audio data
} AudioEntry;

typedef struct {
    uint32_t count;
    AudioEntry* entries;
} Audo;

// ===[ Top-level DataWin container ]===
typedef struct DataWin {
    uint8_t* strgBuffer;        // owned copy of STRG chunk raw data
    // Absolute file offset of strgBuffer[0], we need this because data.win stores absolute offsets (from the beginning of the data.win file) instead of relative offsets
    size_t strgBufferBase;

    uint8_t* bytecodeBuffer;     // owned copy of CODE bytecode blob
    // Absolute file offset of bytecodeBuffer[0], we need this because data.win stores absolute offsets (from the beginning of the data.win file) instead of relative offsets
    size_t bytecodeBufferBase;

    Gen8 gen8;
    Optn optn;
    Lang lang;
    Extn extn;
    Sond sond;
    Agrp agrp;
    Sprt sprt;
    Bgnd bgnd;
    PathChunk path;
    Scpt scpt;
    Glob glob;
    Shdr shdr;
    FontChunk font;
    Tmln tmln;
    Objt objt;
    RoomChunk room;
    // DAFL is empty, no field needed
    Tpag tpag;
    Code code;
    Vari vari;
    Func func;
    Strg strg;
    Txtr txtr;
    Audo audo;

    // Lookup map: absolute file offset -> TPAG index (built during TPAG parsing)
    struct { uint32_t key; int32_t value; }* tpagOffsetMap;
} DataWin;

DataWin* DataWin_parse(const char* filePath, DataWinParserOptions options);
void DataWin_free(DataWin* dataWin);
void DataWin_printDebugSummary(DataWin* dataWin);
int32_t DataWin_resolveTPAG(DataWin* dw, uint32_t offset);
void GamePath_computeInternal(GamePath* path);
PathPositionResult GamePath_getPosition(GamePath* path, double t);
