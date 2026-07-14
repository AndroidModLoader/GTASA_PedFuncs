#include <mod/amlmod.h>
#include <mod/logger.h>
#include <unistd.h>
#include <list>

#include <game_sa/extdata/PedExtender.h>
#include <game_sa/other/TextureDatabase.h>
#include <game_sa/engine/ModelInfo.h>
#include <game_sa/base/Timer.h>
#include <game_sa/other/CutsceneMgr.h>

#include <renderware/RwTexture.h>
#include <renderware/RpMaterial.h>
#include <renderware/RpClump.h>
#include <renderware/RpGeometry.h>
#include <renderware/RpAtomic.h>

MYMOD(net.juniordjjr.rusjj.pedfuncs, PedFuncs, 1.1, JuniorDjjr & RusJJ)
BEGIN_DEPLIST()
    ADD_DEPENDENCY_VER(net.rusjj.aml, 1.3)
    ADD_DEPENDENCY_VER(net.rusjj.gtasa.utils, 1.3)
END_DEPLIST()

#include "isautils.h"
ISAUtils* sautils = NULL;

const int TEXTURE_LIMIT = 32;
const int TEXDB_LIMIT = 4;
const int MAX_PEDS_ID = 22000;

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
    PedExtended(CPed* ped)
    {
        Reset();
    }
};
PedExtendedData<PedExtended> extData;

uintptr_t pGTASA;
void* hGTASA;

// OWN Vars
RwTexture *pTextureHandsBlack, *pTextureHandsWhite;
TextureDatabaseRuntime **GangHandsTexDB;
TextureDatabaseRuntime **PedsRemapDatabases[TEXDB_LIMIT];
uint32_t PedRemapsFunctionalityTime = 0;
int RemapsIdForModelIds[MAX_PEDS_ID + 1];
PedRemaps PossiblePedRemaps[MAX_PEDS_ID + 1];
char PedRemapTexdbNames[TEXDB_LIMIT][32];

// OWN Funcs
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
    return texdb->GetRWTexture(texdb->GetEntry(name, &hasSiblings));
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

    CBaseModelInfo* pedModelInfo = CModelInfo::ms_modelInfoPtrs[modelId];
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
        auto& info = extData.Get(ped);
        if(info.remap->hasRemaps)
        {
            if(info.didChanges)
            {
                info.currentProcessedTexture = 0;
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
                }, &info);
            }
            else
            {
                PedRemaps* remap = info.remap;
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
inline void ChangePedModel_Tweaked(void(*org)(CPed*, int), CPed* self, int model)
{
    bool ready = (CTimer::m_snTimeInMilliseconds > PedRemapsFunctionalityTime);
    auto& info = extData.Get(self);
    info.Reset();
    auto remapData = &PossiblePedRemaps[model];
    info.remap = remapData;

    if(!ready)
    {
        org(self, model);
        return;
    }

    org(self, model);
    PreparePed(self, info);
}
inline void PedRender_Tweaked(void(*org)(CPed*), CPed* self)
{
    bool ready = (CTimer::m_snTimeInMilliseconds > PedRemapsFunctionalityTime);
    if(self->m_nModelIndex != 0 && ready) ProcessPedFuncs(self);
    org(self);
}

// Hooks
DECL_HOOKv(ChangePedModel1, CPed* self, int model) { ChangePedModel_Tweaked(ChangePedModel1, self, model); }
DECL_HOOKv(ChangePedModel2, CPed* self, int model) { ChangePedModel_Tweaked(ChangePedModel2, self, model); }
DECL_HOOKv(ChangePedModel3, CPed* self, int model) { ChangePedModel_Tweaked(ChangePedModel3, self, model); }
DECL_HOOKv(ChangePedModel4, CPed* self, int model) { ChangePedModel_Tweaked(ChangePedModel4, self, model); }
DECL_HOOKv(ChangePedModel5, CPed* self, int model) { ChangePedModel_Tweaked(ChangePedModel5, self, model); }
DECL_HOOKv(ChangePedModel6, CPed* self, int model) { ChangePedModel_Tweaked(ChangePedModel6, self, model); }

DECL_HOOKv(PedRender1, CPed* self) { PedRender_Tweaked(PedRender1, self); }
DECL_HOOKv(PedRender2, CPed* self) { PedRender_Tweaked(PedRender2, self); }
DECL_HOOKv(PedRender3, CPed* self) { PedRender_Tweaked(PedRender3, self); }
DECL_HOOKv(PedRender4, CPed* self) { PedRender_Tweaked(PedRender4, self); }
DECL_HOOKv(PedRender5, CPed* self) { PedRender_Tweaked(PedRender5, self); }

DECL_HOOKv(CutsceneManagerUpdate)
{
    CutsceneManagerUpdate();

    if(CCutsceneMgr::ms_running)
    {
        PedRemapsFunctionalityTime = CTimer::m_snTimeInMilliseconds + 3000;
    }
}

// Patch
uintptr_t HandObjectMissingTexture_BackTo;
extern "C" RwTexture* HandObjectMissingTexture_Patch(CPed* ped)
{
    if(!pTextureHandsBlack && !pTextureHandsWhite && *GangHandsTexDB)
    {
        pTextureHandsBlack = GetTextureFromTexDB(*GangHandsTexDB, "hands_black");
        pTextureHandsWhite = GetTextureFromTexDB(*GangHandsTexDB, "hands_white");

        if(pTextureHandsBlack) ++pTextureHandsBlack->refCount;
        if(pTextureHandsWhite) ++pTextureHandsWhite->refCount;
    }

    CPedModelInfo* pedModelInfo = (CPedModelInfo*)( CModelInfo::ms_modelInfoPtrs[ped->m_nModelIndex] );
    if (pedModelInfo->m_nPedType == ePedType::PED_TYPE_GANG1 || pedModelInfo->m_nPedType == ePedType::PED_TYPE_GANG2)
    {
        return pTextureHandsBlack;
    }
    return pTextureHandsWhite;
}
__attribute__((optnone)) __attribute__((naked)) void HandObjectMissingTexture(void)
{
  #ifdef AML32
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
  #else
    asm volatile("MOV X0, X19\nBL HandObjectMissingTexture_Patch");
    asm volatile("MOV X8, %0\n" :: "r"(HandObjectMissingTexture_BackTo));
    asm("BR X8");
  #endif
}

// Main
extern "C" void OnModLoad()
{
    logger->SetTag("PedFuncs");

    sautils = (ISAUtils*)GetInterface("SAUtils");
    if(!sautils) return;

    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");

    HOOKPLT(ChangePedModel1, pGTASA + BYBIT(0x674560, 0x8474D8));
    HOOKPLT(ChangePedModel2, pGTASA + BYBIT(0x668ABC, 0x831BD8)); // vtable
    HOOKPLT(ChangePedModel3, pGTASA + BYBIT(0x668B30, 0x831CC0)); // vtable
    HOOKPLT(ChangePedModel4, pGTASA + BYBIT(0x668C08, 0x831E70)); // vtable
    HOOKPLT(ChangePedModel5, pGTASA + BYBIT(0x668C80, 0x831F60)); // vtable
    HOOKPLT(ChangePedModel6, pGTASA + BYBIT(0x6692A0, 0x833128)); // vtable

    HOOKPLT(PedRender1, pGTASA + BYBIT(0x668AF0, 0x831C40)); // vtable
    HOOKPLT(PedRender2, pGTASA + BYBIT(0x668B64, 0x831D28)); // vtable
    HOOKPLT(PedRender3, pGTASA + BYBIT(0x668C3C, 0x831ED8)); // vtable
    HOOKPLT(PedRender4, pGTASA + BYBIT(0x668CB4, 0x831FC8)); // vtable
    HOOKPLT(PedRender5, pGTASA + BYBIT(0x6692D4, 0x833190)); // vtable

    HOOKPLT(CutsceneManagerUpdate, pGTASA + BYBIT(0x6750C8, 0x848718)); // vtable

    // CHandObject::CHandObject
    aml->Redirect(pGTASA + BYBIT(0x4529B6 + 0x1, 0x53B4F0), (uintptr_t)HandObjectMissingTexture);
    HandObjectMissingTexture_BackTo = pGTASA + BYBIT(0x4529D0 + 0x1, 0x53B510);

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