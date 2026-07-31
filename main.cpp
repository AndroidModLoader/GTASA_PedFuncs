#include <mod/amlmod.h>
#include <mod/logger.h>
#include <unistd.h>
#include <vector>

#include <game_sa/extdata/PedExtender.h>
#include <game_sa/other/TextureDatabase.h>
#include <game_sa/engine/ModelInfo.h>
#include <game_sa/base/Timer.h>
#include <game_sa/other/CutsceneMgr.h>

#include <game_sa/Events.h>

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

const int TEXTURE_LIMIT = 16;
const int TEXDB_LIMIT = 4;
const int MAX_PEDS_ID = 22000;
const int MAX_BACKUPS = 64;
const int MAX_SCAN_DEEP = 5;

struct PedRemaps
{
    std::vector<RwTexture*> vecTextures[TEXTURE_LIMIT];

    int boundModel;
    bool remapsReady;
    bool hasRemaps;
    uint8_t processedMaterial;
    uint8_t materialsNum;

    inline void Init(int model)
    {
        boundModel = model;
        remapsReady = false;
        hasRemaps = false;
        processedMaterial = 0;

        for(int i = 0; i < TEXTURE_LIMIT; ++i)
        {
            vecTextures[i].clear();
        }
    }
    inline int GetRemapsCount(uint8_t matIndex)
    {
        return vecTextures[matIndex].size();
    }
    inline RwTexture* GetRemap(uint8_t matIndex)
    {
        const std::vector<RwTexture*>& vecRemap = vecTextures[matIndex];
        const int size = vecRemap.size();

        if(size <= 0) return NULL;
        return vecRemap[randint(0, size - 1)];
    }
};
struct PedExtended
{
    PedRemaps* remap;
    CPed* myPed;
    RwTexture* remappedTexture[TEXTURE_LIMIT];
    
    uint8_t processedMaterial;
    bool wasProcessedForRemaps;

    PedExtended(CPed* ped)
    {
        Reset();
        myPed = ped;
    }
    inline void Reset()
    {
        remap = NULL;
        myPed = NULL;
        processedMaterial = 0;
        wasProcessedForRemaps = false;

        for(int i = 0; i < TEXTURE_LIMIT; ++i)
        {
            remappedTexture[i] = NULL;
        }
    }
    inline void AssignRemaps()
    {
        for(int i = 0; i < TEXTURE_LIMIT; ++i)
        {
            remappedTexture[i] = remap->GetRemap(i);
        }
        wasProcessedForRemaps = true;
    }
};
struct RemapBackup
{
    RpMaterial* pMaterial;
    RwTexture* pOriginalTexture;
};

PedExtendedData<PedExtended> extData;
RemapBackup backups[MAX_BACKUPS];
int backupsCount = 0;
inline RemapBackup* PushBackup(RpMaterial* mat)
{
    // Material and texture should be guaranteed
    if(backupsCount >= MAX_BACKUPS) return NULL;
    backups[backupsCount].pMaterial = mat;
    backups[backupsCount].pOriginalTexture = mat->texture;
    return &backups[backupsCount++];
}
inline void ResetBackups()
{
    backupsCount = 0;
    // Cleaning pointers there is gonna be waste of CPU cycles
}
inline void FlushBackups()
{
    for(int i = backupsCount - 1; i >= 0; --i)
    {
        backups[i].pMaterial->texture = backups[i].pOriginalTexture;
    }
    ResetBackups();
}

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
inline RwTexture* GetTextureFromTexDB(TextureDatabaseRuntime* texdb, const char* name)
{
    bool hasSiblings = false;
    return texdb->GetRWTexture(texdb->GetEntry(name, &hasSiblings));
}
inline RwTexture* GetTextureFromPedDBs(const char* name)
{
    TextureDatabaseRuntime** pTexDB = NULL;
    RwTexture* texture = NULL;
    for(int i = 0; i < TEXDB_LIMIT; ++i)
    {
        pTexDB = PedsRemapDatabases[i];
        if(!pTexDB || !*pTexDB) continue;

        texture = GetTextureFromTexDB(*pTexDB, name);
        if(texture != NULL) return texture;
    }
    return NULL;
}
inline void FillRemaps(PedRemaps* remap, const char* texName)
{
    char remapTex[rwTEXTUREBASENAMELENGTH], remapNumTex[rwTEXTUREBASENAMELENGTH];
    int deepness = 0, scanNum = 0;

    sprintf(remapTex, "%s_remap", texName);
    while(deepness < MAX_SCAN_DEEP)
    {
        sprintf(remapNumTex, "%s%d", remapTex, scanNum);
        RwTexture* remapTexture = GetTextureFromPedDBs(remapNumTex);
        if(remapTexture != NULL)
        {
            ++(remapTexture->refCount);
            remap->hasRemaps = true;
            remap->vecTextures[remap->processedMaterial].push_back(remapTexture);

            deepness = 0;
        }
        else
        {
            ++deepness;
        }
        ++scanNum;
    }
}
inline void PreparePed(CPed* ped, PedExtended &info)
{
    int modelId = ped->m_nModelIndex;
    if(!modelId) return;

    CBaseModelInfo* pedModelInfo = CModelInfo::ms_modelInfoPtrs[modelId];
    if(!pedModelInfo) return;
    
    auto clump = ped->m_pRwClump;
    if(!clump || clump->object.type != rpCLUMP) return;
    
    PedRemaps* remap = info.remap;

    // Get all possible remaps for MODEL ID (once)
    if(!remap->remapsReady)
    {
        remap->processedMaterial = 0;
        RpClumpForAllAtomics(clump, [](RpAtomic *atomic, void *data)
        {
            if(!atomic->geometry) return atomic;
            
            RpGeometryForAllMaterials(atomic->geometry, [](RpMaterial *material, void *data)
            {
                PedRemaps* remap = (PedRemaps*)data;
                if(remap->processedMaterial < TEXTURE_LIMIT && material && material->texture)
                {
                    FillRemaps(remap, material->texture->name);
                }
                ++remap->processedMaterial;
                return material;
            }, data);
            return atomic;
        }, remap);

        remap->remapsReady = true;
        remap->materialsNum = remap->processedMaterial;
    }

    if(remap->hasRemaps) info.AssignRemaps();
    info.wasProcessedForRemaps = true;
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
    if(!clump || clump->object.type != rpCLUMP) return;
    
    auto& info = extData.Get(ped);
    if(!info.wasProcessedForRemaps)
    {
        PreparePed(ped, info);
    }
    if(!info.remap->hasRemaps) return;
    
    info.processedMaterial = 0;
    RpClumpForAllAtomics(clump, [](RpAtomic *atomic, void *data)
    {
        if(!atomic->geometry) return atomic;
        
        RpGeometryForAllMaterials(atomic->geometry, [](RpMaterial *material, void *data)
        {
            PedExtended* info = (PedExtended*)data;
            int matIdx = info->processedMaterial;
            if(matIdx < TEXTURE_LIMIT && material && material->texture)
            {
                RwTexture* remapTex = info->remappedTexture[matIdx];
                if(remapTex && PushBackup(material))
                {
                    material->texture = remapTex;
                }
            }
            ++info->processedMaterial;
            return material;
        }, data);

        return atomic;
    }, &info);
}
inline void ProcessPedFuncsPost(CPed* ped)
{
    FlushBackups();
}
inline void ChangePedModel_Tweaked(void(*orgFn)(CPed*, int), CPed* self, int model)
{
    auto& info = extData.Get(self);
    info.Reset();
    PedRemaps* remap = &PossiblePedRemaps[model];
    info.remap = remap;

    orgFn(self, model);

    if(model != 0 && CTimer::GetTimeMS() > PedRemapsFunctionalityTime)
    {
        PreparePed(self, info);
    }
}
inline void PedRender_Tweaked(void(*orgFn)(CPed*), CPed* self)
{
    bool bCanRemap = (self->m_nModelIndex != 0 && CTimer::GetTimeMS() > PedRemapsFunctionalityTime);
    if(!bCanRemap) return orgFn(self);
    
    ProcessPedFuncs(self);
    orgFn(self);
    ProcessPedFuncsPost(self);
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

// Patch
uintptr_t HandObjectMissingTexture_BackTo;
extern "C" RwTexture* HandObjectMissingTexture_Patch(CPed* ped)
{
    if(!pTextureHandsBlack && !pTextureHandsWhite && *GangHandsTexDB)
    {
        pTextureHandsBlack = GetTextureFromTexDB(*GangHandsTexDB, "hands_black");
        pTextureHandsWhite = GetTextureFromTexDB(*GangHandsTexDB, "hands_white");

        if(pTextureHandsBlack) ++(pTextureHandsBlack->refCount);
        if(pTextureHandsWhite) ++(pTextureHandsWhite->refCount);
    }

    CPedModelInfo* pedModelInfo = (CPedModelInfo*)( CModelInfo::ms_modelInfoPtrs[ped->m_nModelIndex] );
    if(pedModelInfo && (
        pedModelInfo->m_nPedType == ePedType::PED_TYPE_GANG1 || pedModelInfo->m_nPedType == ePedType::PED_TYPE_GANG2
    ) )
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
ON_MOD_LOAD()
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

    Events::processScriptsEvent += []()
    {
        if(CCutsceneMgr::ms_running)
        {
            PedRemapsFunctionalityTime = CTimer::GetTimeMS() + 3000;
        }
    };

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
        PossiblePedRemaps[i].Init(i);
    }
}