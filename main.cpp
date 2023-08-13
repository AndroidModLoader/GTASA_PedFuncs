#include <mod/amlmod.h>
#include <mod/logger.h>
#include <unistd.h>
#include <list>

#ifdef AML32
    #include "GTASA_STRUCTS.h"
    #define BYVER(__for32, __for64) (__for32)
#else
    #include "GTASA_STRUCTS_210.h"
    #define BYVER(__for32, __for64) (__for64)
#endif

MYMOD(net.juniordjjr.rusjj.pedfuncs, PedFuncs, 1.0.2, JuniorDjjr & RusJJ)
BEGIN_DEPLIST()
    ADD_DEPENDENCY_VER(net.rusjj.aml, 1.0.2.1)
    ADD_DEPENDENCY_VER(net.rusjj.gtasa.utils, 1.3)
END_DEPLIST()

#include "isautils.h"
ISAUtils* sautils = NULL;

const int TEXTURE_LIMIT = 32;
const int TEXDB_LIMIT = 4;
const int MAX_PEDS_ID = 20000;

struct PedRemaps
{
    bool thisModelProcessed;
    bool hasRemaps;
    uint8_t currentProcessedTexture;
    RwTexture* originalTextures[TEXTURE_LIMIT];
    char remapTexturesName[TEXTURE_LIMIT][rwTEXTUREBASENAMELENGTH];
    uint8_t remapsCount[TEXTURE_LIMIT];

    void Init()
    {
        thisModelProcessed = false;
        hasRemaps = false;
        for(int i = 0; i < TEXTURE_LIMIT; ++i)
        {
            originalTextures[i] = NULL;
            remapTexturesName[i][0] = 0;
            remapsCount[i] = 0;
        }
    }
};
struct PedExtended
{
    bool didChanges;
    uint8_t currentProcessedTexture;
    RwTexture* remappedTexture[TEXTURE_LIMIT];
    PedRemaps* remap;

    inline void Reset()
    {
        didChanges = false;
        currentProcessedTexture = 0;
        remap = NULL;
    }
};

uintptr_t pGTASA;
void* hGTASA;

// GTA Vars
CPool<CPed, CCopPed> **ms_pPedPool;
CBaseModelInfo** ms_modelInfoPtrs;
uint32_t *m_snTimeInMilliseconds;
bool *ms_running;

// GTA Funcs
int (*GetEntry)(TextureDatabaseRuntime *,char const*, bool*);
RwTexture* (*GetRWTexture)(TextureDatabaseRuntime *, int);
void (*RpClumpForAllAtomics)(RpClump *,RpAtomic * (*)(RpAtomic *,void *),void *);
void (*RpGeometryForAllMaterials)(RpGeometry *,RpMaterial * (*)(RpMaterial *,void *),void *);
RwTexture* (*GetTexture)(const char *);
RpClump* (*RpClumpClone)(RpClump *);
void (*RpClumpDestroy)(RpClump *);

// OWN Vars
CPool<PedExtended> *ms_pPedExtendedPool = NULL;
RwTexture *pTextureHandsBlack, *pTextureHandsWhite;
TextureDatabaseRuntime **GangHandsTexDB;
TextureDatabaseRuntime **PedsRemapDatabases[TEXDB_LIMIT];
uint32_t LastCutsceneEnded = 0;
int RemapsIdForModelIds[MAX_PEDS_ID + 1];
PedRemaps PossiblePedRemaps[MAX_PEDS_ID + 1];
char PedRemapTexdbNames[TEXDB_LIMIT][32];

// OWN Configs


// OWN Funcs
inline PedExtended* GetExtData(CPed* ped)
{
    if(!ms_pPedExtendedPool)
    {
        if(!*ms_pPedPool) return NULL;
        auto size = (*ms_pPedPool)->m_nSize;
        ms_pPedExtendedPool = new CPool<PedExtended>(size, "PedExtended");
        ms_pPedExtendedPool->m_nFirstFree = size;
        for(int i = 0; i < size; ++i) ms_pPedExtendedPool->m_byteMap[i].bEmpty = false;
    }
    return ms_pPedExtendedPool->GetAt((*ms_pPedPool)->GetIndex(ped));
}
inline void Clamp(int& val, int min, int max)
{
    if(val > max) val = max;
    else if(val < min) val = min;
}
inline void Clamp(float& val, float min, float max)
{
    if(val > max) val = max;
    else if(val < min) val = min;
}
inline int RandomInt(int min, int max)
{
    int r = max - min + 1;
    return min + rand() % r;
}
inline int RandomIntEx(int min, int max)
{
    int r = max - min;
    return min + rand() % r;
}
inline int RandomFromZero(int max)
{
    int r = max + 1;
    return rand() % r;
}
inline float RandomFloat(float min, float max)
{
    float r = (float)rand() / (float)RAND_MAX;
    return min + r * (max - min);
}
inline RwTexture* GetTextureFromTexDB(TextureDatabaseRuntime* texdb, const char* name)
{
    bool hasSiblings;
    return GetRWTexture(texdb, GetEntry(texdb, name, &hasSiblings));
}
inline RwTexture* GetTextureFromPedDBs(const char* name)
{
    TextureDatabaseRuntime** pTexdb = NULL;
    RwTexture* texture = NULL;
    for(int i = 0; i < TEXDB_LIMIT; ++i)
    {
        pTexdb = PedsRemapDatabases[i];
        if(!pTexdb || !*pTexdb) continue;
        texture = GetTextureFromTexDB(*pTexdb, name);
        if(texture != NULL) return texture;
    }
    return NULL;
}
PedExtended* CurrentPedExtended;
inline void PreparePed(CPed* ped, PedExtended &info)
{
    int modelId = ped->m_nModelIndex;
    if(!modelId) return;

    CBaseModelInfo* pedModelInfo = ms_modelInfoPtrs[modelId];
    if(pedModelInfo)
    {
        auto clump = ped->m_pRwClump;
        if (clump && clump->object.type == rpCLUMP)
        {
            PedRemaps* remapData = info.remap;
            if(!remapData->thisModelProcessed)
            {
                remapData->currentProcessedTexture = 0;
                RpClumpForAllAtomics(clump, [](RpAtomic *atomic, void *data)
                {
                    if (atomic->geometry)
                    {
                        RpGeometryForAllMaterials(atomic->geometry, [](RpMaterial *material, void *data)
                        {
                            PedRemaps* remap = (PedRemaps*)data;
                            int i = remap->currentProcessedTexture; ++remap->currentProcessedTexture;
                            if(!material || !material->texture) return material;
                            remap->originalTextures[i] = material->texture;

                            char remapTex[rwTEXTUREBASENAMELENGTH], remapNumTex[rwTEXTUREBASENAMELENGTH];
                            sprintf(remapTex, "%s_remap", material->texture->name);
                            strcpy(remap->remapTexturesName[i], remapTex);

                            sprintf(remapNumTex, "%s%d", remapTex, remap->remapsCount[i] + 1);
                            RwTexture* remapTexture = GetTextureFromPedDBs(remapNumTex);
                            while(remapTexture)
                            {
                                remap->hasRemaps = true;
                                ++remap->remapsCount[i];
                                sprintf(remapNumTex, "%s%d", remapTex, remap->remapsCount[i] + 1);
                                remapTexture = GetTextureFromPedDBs(remapNumTex);
                            }
                            return material;
                        }, data);
                    }
                    return atomic;
                }, remapData);
                remapData->thisModelProcessed = true;
            }

            if(remapData->hasRemaps)
            {
                CurrentPedExtended = &info;
                remapData->currentProcessedTexture = 0;
                RpClumpForAllAtomics(clump, [](RpAtomic *atomic, void *data)
                {
                    if (atomic->geometry)
                    {
                        RpGeometryForAllMaterials(atomic->geometry, [](RpMaterial *material, void *data)
                        {
                            PedRemaps* remap = (PedRemaps*)data;
                            int i = remap->currentProcessedTexture; ++remap->currentProcessedTexture;
                            if(!material || !material->texture) return material;
                            if(remap->remapsCount[i] > 0)
                            {
                                int remapNum = RandomFromZero(remap->remapsCount[i]);
                                if(remapNum != 0)
                                {
                                    char remapTex[rwTEXTUREBASENAMELENGTH];
                                    sprintf(remapTex, "%s%d", remap->remapTexturesName[i], remapNum);
                                    RwTexture* texture = GetTextureFromPedDBs(remapTex);
                                    if(texture)
                                    {
                                        CurrentPedExtended->didChanges = true;
                                        CurrentPedExtended->remappedTexture[i] = texture;
                                        ++texture->refCount;
                                        return material;
                                    }
                                }
                            }
                            CurrentPedExtended->remappedTexture[i] = remap->originalTextures[i];
                            return material;
                        }, data);
                    }
                    return atomic;
                }, remapData);
            }
        }
    }
}
inline TextureDatabaseRuntime** LoadDBIfExists(const char* name, bool registerTo = false)
{
    char path[256];
    sprintf(path, "%s/texdb/%s/%s.txt", aml->GetAndroidDataPath(), name, name);
    return (access(path, F_OK) != 0) ? NULL : (TextureDatabaseRuntime**)sautils->AddTextureDB(name, registerTo);
}
inline void ProcessPedFuncs(CPed* ped)
{
    auto clump = ped->m_pRwClump;
    if (clump && clump->object.type == rpCLUMP)
    {
        auto info = GetExtData(ped);
        logger->Info("ProcessPedFuncs: %d, hasRemaps %d", ped->m_nModelIndex, info->remap->hasRemaps);
        if(info->remap->hasRemaps)
        {
            if(info->didChanges)
            {
                info->currentProcessedTexture = 0;
                RpClumpForAllAtomics(clump, [](RpAtomic *atomic, void *data)
                {
                    if (atomic->geometry)
                    {
                        RpGeometryForAllMaterials(atomic->geometry, [](RpMaterial *material, void *data)
                        {
                            PedExtended* info = (PedExtended*)data;
                            int i = info->currentProcessedTexture; ++info->currentProcessedTexture;
                            if(!material || !material->texture) return material;

                            material->texture = info->remappedTexture[i];
                            return material;
                        }, data);
                    }
                    return atomic;
                }, info);
            }
            else
            {
                PedRemaps* remap = info->remap;
                remap->currentProcessedTexture = 0;
                RpClumpForAllAtomics(clump, [](RpAtomic *atomic, void *data)
                {
                    if (atomic->geometry)
                    {
                        RpGeometryForAllMaterials(atomic->geometry, [](RpMaterial *material, void *data)
                        {
                            PedRemaps* remap = (PedRemaps*)data;
                            int i = remap->currentProcessedTexture; ++remap->currentProcessedTexture;
                            if(!material || !material->texture) return material;

                            RwTexture *orgTexture = remap->originalTextures[i];
                            if(!orgTexture) remap->originalTextures[i] = material->texture;
                            else material->texture = orgTexture;
                            return material;
                        }, data);
                    }
                    return atomic;
                }, remap);
            }
        }
    }
}

// Hooks
DECL_HOOKv(ChangePedModel, CPed* self, int model)
{
    bool ready = (*m_snTimeInMilliseconds > LastCutsceneEnded);
    auto& info = *GetExtData(self);
    info.Reset();
    auto remapData = &PossiblePedRemaps[model];
    info.remap = remapData;

    if(!ready)
    {
        ChangePedModel(self, model);
        return;
    }

    ChangePedModel(self, model);
    PreparePed(self, info);
}
DECL_HOOKv(PedRender, CPed* self)
{
    if(self->m_nModelIndex != 0) ProcessPedFuncs(self);
    PedRender(self);
}
DECL_HOOKv(CutsceneManagerUpdate)
{
    CutsceneManagerUpdate();
    if(*ms_running) LastCutsceneEnded = *m_snTimeInMilliseconds + 3000;
}

// Patch
uintptr_t HandObjectMissingTexture_BackTo;
extern "C" RwTexture* HandObjectMissingTexture_Patch(CPed* ped)
{
    if(!pTextureHandsBlack)
    {
        pTextureHandsBlack = GetTextureFromTexDB(*GangHandsTexDB, "hands_black");
        pTextureHandsWhite = GetTextureFromTexDB(*GangHandsTexDB, "hands_white");

        ++pTextureHandsBlack->refCount;
        ++pTextureHandsWhite->refCount;
    }

    CPedModelInfo* pedModelInfo = (CPedModelInfo*)(ms_modelInfoPtrs[ped->m_nModelIndex]);
    if (pedModelInfo->m_defaultPedType == ePedType::PED_TYPE_GANG1 || pedModelInfo->m_defaultPedType == ePedType::PED_TYPE_GANG2) return pTextureHandsBlack;
    return pTextureHandsWhite;
}
#ifdef AML32
__attribute__((optnone)) __attribute__((naked)) void HandObjectMissingTexture(void)
{
    asm volatile(
        "PUSH {R1-R11}\n"
        "LDR R0, [SP, #0x14]\n"
        "BL HandObjectMissingTexture_Patch\n"
        "PUSH {R0}\n"
    );

    asm volatile(
        "MOV R12, %0\n"
        "POP {R0}\n"
        "POP {R1-R11}\n"
        "BX R12\n"
    :: "r" (HandObjectMissingTexture_BackTo));
}
#else
__attribute__((optnone)) __attribute__((naked)) void HandObjectMissingTexture(void)
{
    asm volatile("MOV X0, X19\nBL HandObjectMissingTexture_Patch");
    asm volatile("MOV X8, %0\n" :: "r"(HandObjectMissingTexture_BackTo));
    asm("BR X8");
}
#endif

// Main
extern "C" void OnModLoad()
{
    logger->SetTag("PedFuncs");

    sautils = (ISAUtils*)GetInterface("SAUtils");
    if(!sautils) return;

    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");

    SET_TO(GetEntry, aml->GetSym(hGTASA, "_ZN22TextureDatabaseRuntime8GetEntryEPKcRb"));
    SET_TO(GetRWTexture, aml->GetSym(hGTASA, "_ZN22TextureDatabaseRuntime12GetRWTextureEi"));
    SET_TO(RpClumpForAllAtomics, aml->GetSym(hGTASA, "_Z20RpClumpForAllAtomicsP7RpClumpPFP8RpAtomicS2_PvES3_"));
    SET_TO(RpGeometryForAllMaterials, aml->GetSym(hGTASA, "_Z25RpGeometryForAllMaterialsP10RpGeometryPFP10RpMaterialS2_PvES3_"));
    SET_TO(GetTexture, aml->GetSym(hGTASA, "_ZN22TextureDatabaseRuntime10GetTextureEPKc"));
    SET_TO(RpClumpClone, aml->GetSym(hGTASA, "_Z12RpClumpCloneP7RpClump"));
    SET_TO(RpClumpDestroy, aml->GetSym(hGTASA, "_Z14RpClumpDestroyP7RpClump"));

    SET_TO(ms_pPedPool, aml->GetSym(hGTASA, "_ZN6CPools11ms_pPedPoolE"));
    SET_TO(ms_modelInfoPtrs, aml->GetSym(hGTASA, "_ZN10CModelInfo16ms_modelInfoPtrsE"));
    SET_TO(m_snTimeInMilliseconds, aml->GetSym(hGTASA, "_ZN6CTimer22m_snTimeInMillisecondsE"));
    SET_TO(ms_running, aml->GetSym(hGTASA, "_ZN12CCutsceneMgr10ms_runningE"));

    HOOKPLT(ChangePedModel, pGTASA + BYVER(0x674560, 0x8474D8));
    HOOKPLT(ChangePedModel, pGTASA + BYVER(0x668ABC, 0x831BD8)); // vtable
    HOOKPLT(ChangePedModel, pGTASA + BYVER(0x668B30, 0x831CC0)); // vtable
    HOOKPLT(ChangePedModel, pGTASA + BYVER(0x668C08, 0x831E70)); // vtable
    HOOKPLT(ChangePedModel, pGTASA + BYVER(0x668C80, 0x831F60)); // vtable
    HOOKPLT(ChangePedModel, pGTASA + BYVER(0x6692A0, 0x833128)); // vtable

    HOOKPLT(PedRender, pGTASA + BYVER(0x668AF0, 0x831C40)); // vtable
    HOOKPLT(PedRender, pGTASA + BYVER(0x668B64, 0x831D28)); // vtable
    HOOKPLT(PedRender, pGTASA + BYVER(0x668C3C, 0x831ED8)); // vtable
    HOOKPLT(PedRender, pGTASA + BYVER(0x668CB4, 0x831FC8)); // vtable
    HOOKPLT(PedRender, pGTASA + BYVER(0x6692D4, 0x833190)); // vtable

    HOOKPLT(CutsceneManagerUpdate, pGTASA + BYVER(0x675B90, 0x8498B8)); // vtable

    // CHandObject::CHandObject
    aml->Redirect(pGTASA + BYVER(0x4529B6 + 0x1, 0x53B4F0), (uintptr_t)HandObjectMissingTexture);
    HandObjectMissingTexture_BackTo = pGTASA + BYVER(0x4529D0 + 0x1, 0x53B510);

    GangHandsTexDB = (TextureDatabaseRuntime**)sautils->AddTextureDB("ganghands");

    for(int i = 0; i < TEXDB_LIMIT; ++i)
    {
        sprintf(PedRemapTexdbNames[i], "peds%d", i + 1);
        PedsRemapDatabases[i] = LoadDBIfExists(PedRemapTexdbNames[i]);
    }
    for(int i = 0; i <= MAX_PEDS_ID; ++i)
    {
        PossiblePedRemaps[i].Init();
    }
}