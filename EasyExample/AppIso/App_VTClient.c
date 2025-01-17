/* ************************************************************************ */
/*!
   \file           

   \brief      VT Client application  

   \author     Erwin Hammerl
   \date       Created 17.09.2007 
   \copyright  Wegscheider Hammerl Ingenieure Partnerschaft

   \par HISTORY:

*/

/* **************************  includes ********************************** */


#include "IsoDef.h"
#include "Common/IsoUtil.h"

#ifdef _LAY6_  /* compile only if VT client is enabled */

#include "Settings/settings.h"
#include "AppMemAccess.h"
#include "AppCommon/AppOutput.h"

#include "App_VTClient.h"
#include "VIEngine.h"
#include "App_VTClientLev2.h"

#include "MyProject1.iop.h"


#if defined(linux)
#include "pools/pool.iop.h"
#endif // defined(linux)

#if defined(_LAY6_) && defined(ISO_VTC_GRAPHIC_AUX)
#include "../Samples/VtcWithAuxPoolUpload/GAux.h"
#endif // defined(_LAY6_) && defined(ISO_VTC_GRAPHIC_AUX)

/* ****************************** global data   *************************** */
static iso_s16  s16_CfHndVtClient = HANDLE_UNVALID;      // Stored CF handle of VT client
static iso_u8   u8_CfVtInstance = ISO_INSTANCE_INVALID;  // Instance number of the VT client
static iso_u8   u8_CfAuxVtInstance = ISO_INSTANCE_INVALID;  // Instance number of the Aux VT client

/* ****************************** function prototypes ****************************** */
static void CbVtConnCtrl        (const ISOVT_EVENT_DATA_T* psEvData);
static void CbVtStatus          (const ISOVT_STATUS_DATA_T* psStatusData);
static void CbVtMessages        (const ISOVT_MSG_STA_T * pIsoMsgSta);
static void CbAuxPrefAssignment (VT_AUXAPP_T asAuxAss[], iso_s16* ps16MaxNumberOfAssigns, ISO_USER_PARAM_T userParam);

static void AppPoolSettings     (iso_u8 u8Instance);
static void AppVTClientDoProcess(void);


/* ************************************************************************ */
void AppVTClientLogin(iso_s16 s16CfHandle)
{
   ISO_USER_PARAM_T  userParamVt = ISO_USER_PARAM_DEFAULT;
   uint64_t u64Name = 0;
   iso_u8 u8BootTime = 0;
   ISO_CF_NAME_T     au8NamePreferredVT = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

   u64Name = getX64("CF-A", "preferredVT", 0xFFFFFFFFFFFFFFFFU);
   au8NamePreferredVT[0] = (iso_u8)( u64Name        & 0xFFU);
   au8NamePreferredVT[1] = (iso_u8)((u64Name >>  8) & 0xFFU);
   au8NamePreferredVT[2] = (iso_u8)((u64Name >> 16) & 0xFFU);
   au8NamePreferredVT[3] = (iso_u8)((u64Name >> 24) & 0xFFU);
   au8NamePreferredVT[4] = (iso_u8)((u64Name >> 32) & 0xFFU);
   au8NamePreferredVT[5] = (iso_u8)((u64Name >> 40) & 0xFFU);
   au8NamePreferredVT[6] = (iso_u8)((u64Name >> 48) & 0xFFU);
   au8NamePreferredVT[7] = (iso_u8)((u64Name >> 56) & 0xFFU);

   u8BootTime = getU8("CF-A", "bootTimeVT", 7u);

   // Initialize the VT client instance
   u8_CfVtInstance = IsoVT_CreateInstance(s16CfHandle, userParamVt, CbVtStatus, CbVtMessages, CbVtConnCtrl,
                                          CAST_TO_CONST_ISONAME_PTR(&au8NamePreferredVT));
   (void)IsoVtcDataSet(u8_CfVtInstance, VT_BOOTTIME, u8BootTime);   // Set (EE-stored) boot time of the preferred VT (in seconds)
   
   // Use preferred assignment callback function, which is called before sending the preferred assignment for the auxiliary functions to the VT
   (void)IsoVtcAuxPrefAssignmentCbSet(u8_CfVtInstance, &CbAuxPrefAssignment);

#if defined(_LAY6_) && defined(ISO_VTC_GRAPHIC_AUX)
   /* Add CF to graphical aux implements */
   (void)vtcGAux_CfInit(s16CfHandle, userParamVt, "pools/Pool.iop");
#endif // defined(_LAY6_) && defined(ISO_VTC_GRAPHIC_AUX)

   s16_CfHndVtClient = s16CfHandle; // Store VT client CF handle
}

/* ************************************************************************ */
static void CbVtConnCtrl(const ISOVT_EVENT_DATA_T* psEvData)
{
   iso_u8 abLCData[6];

   switch (psEvData->eEvent)
   {
   case IsoEvConnSelectPreferredVT:
      /* preferred VT is not alive, but one or more other VTs */
      VTC_setNewVT(psEvData->u8Instance);
      break;
   case IsoEvMaskServerVersAvailable:
      if (IsoVtcGetStatusInfo(psEvData->u8Instance, VT_VERSIONNR) >= 4u)
      {
         // IsoVTObjTypeParsableSet(PNGObject);  // for test purposes (must be called here)
      }
      break;
   case IsoEvMaskLanguageCmd:
      IsoVtcReadWorkingSetLanguageData(psEvData->u8Instance, abLCData);
      break;
   case IsoEvMaskTechDataV4Request:
      /* If VT >= V4 then application can request some more technical data */
      if (IsoVtcGetStatusInfo(psEvData->u8Instance, WS_VERSION_NR) >= 4u)
      {
         IsoVtcCmd_GetSupportedObjects(psEvData->u8Instance);
         IsoVtcCmd_GetWindowMaskData(psEvData->u8Instance);
         //IsoVtcCmd_GetSupportedWidechar(psEvData->u8Instance, ...)
      }
      break;
   case IsoEvMaskLoadObjects:
      AppPoolSettings(psEvData->u8Instance);
      {  /* Current VT and boot time of VT can be read and stored here in EEPROM */
         iso_s16 s16HndCurrentVT = (iso_s16)IsoVtcGetStatusInfo(psEvData->u8Instance, VT_HND);   /* get CF handle of actual VT */
         ISO_CF_INFO_T cfInfo = {0};
         iso_s16 s16Err = iso_NmGetCfInfo( s16HndCurrentVT, &cfInfo );
         if (s16Err == E_NO_ERR)
         {
            uint64_t u64Name = ((uint64_t)(cfInfo.au8Name[0]))       |
                               ((uint64_t)(cfInfo.au8Name[1]) <<  8) |
                               ((uint64_t)(cfInfo.au8Name[2]) << 16) |
                               ((uint64_t)(cfInfo.au8Name[3]) << 24) |
                               ((uint64_t)(cfInfo.au8Name[4]) << 32) |
                               ((uint64_t)(cfInfo.au8Name[5]) << 40) |
                               ((uint64_t)(cfInfo.au8Name[6]) << 48) |
                               ((uint64_t)(cfInfo.au8Name[7]) << 56);
            setX64("CF-A", "preferredVT", u64Name);

            iso_u8 u8BootTime = (iso_u8)IsoVtcGetStatusInfo (psEvData->u8Instance, VT_BOOTTIME );
            setU8("CF-A", "bootTimeVT", u8BootTime);
         }
      }
      break;
   case IsoEvMaskReadyToStore:
      /* pool upload finished - here we can change objects values which should be stored */
      break;
   case IsoEvMaskActivated:
      /* pool is ready - here we can setup the initial mask and data which should be displayed */
	  VTC_setPoolReady(psEvData->u8Instance);
      break;
   case IsoEvMaskTick:  // Cyclic event; Called only after successful login
      AppVTClientDoProcess();   // Sending of commands etc. for mask instance
      break;
   case IsoEvMaskLoginAborted:
      // Login failed - application has to decide if login shall be repeated and how often
      //AppVTClientLogin(s16_CfHndVtClient);
      break;
   case IsoEvConnSafeState:
      // Connection closed ( VT lost, VT_LOGOUT (delete object pool response was received ) )
      break;
   case IsoEvAuxServerVersAvailable:
      break;
   case IsoEvAuxLanguageCmd:
      //IsoClServ_ReadLCOfServer( , );
      break;
   case IsoEvAuxTechDataV4Request:
      break;
   case IsoEvAuxLoadObjects:
      u8_CfAuxVtInstance = psEvData->u8Instance;
      AppPoolSettings(psEvData->u8Instance);
      break;
   case IsoEvAuxActivated:
      break;
   case IsoEvAuxTick:
      break;
   case IsoEvAuxLoginAborted:
      // Login failed - application has to decide if login shall be repeated and how often
      u8_CfAuxVtInstance = ISO_INSTANCE_INVALID;
      break;
   default:
      break;
   }
}

/* ************************************************************************ */
static void AppVTClientDoProcess( void )
{  /* Cyclic VTClient function */

}


#if defined(ESP_PLATFORM)
static const char *POOL_FILENAME = "/spiffs/pools/MyProject1.iop";
#else
static const char *POOL_FILENAME = "pools/MyProject1.iop";
#endif // defined(ESP_PLATFORM)


/* ************************************************************************ */
static void AppPoolSettings( iso_u8 u8Instance )
{
   static iso_u8*  pu8PoolData = 0;        // Pointer to the pool data ( Attention:  )
   static iso_u32  u32PoolSize = 0UL;
   static iso_u16  u16NumberObjects;
   
	u32PoolSize = LoadPoolFromFile(POOL_FILENAME, &pu8PoolData);

#if defined(linux)
	if ((u32PoolSize == 0) || (pu8PoolData == 0))
    {
        pu8PoolData = (iso_u8*)&pool_iop[0];
        u32PoolSize = sizeof(pool_iop);
    }
#endif // defined(linux)

	u16NumberObjects = IsoGetNumofPoolObjs(pu8PoolData, (iso_s32)u32PoolSize);

   {  // Set pool information


	   (void)IsoVtcPoolInit( u8Instance, (const iso_u8*) ISO_VERSION_LABEL, pu8PoolData, 0,       // Instance, Version, PoolAddress, ( PoolSize not needed )
		                   u16NumberObjects, colour_256,     // Number of objects, Graphic typ, 
						   ISO_DESIGNATOR_WIDTH, ISO_DESIGNATOR_HEIGHT, ISO_MASK_SIZE  );                   // SKM width and height, DM res.
   }

   // Set pool manipulations
   VTC_setPoolManipulation( u8Instance );

}

/* ************************************************************************ */
/* This function is called in case of every page change - you can do e. g. initialisations ...  */
static void CbVtStatus(const ISOVT_STATUS_DATA_T* psStatusData)
{
   switch (psStatusData->wPage)
   {
   case DataMask_Home:
      break;

   default:
       break;
   }
}


/* ************************************************************************ */
/*!                                                                               
   \brief       Receiving all messages of VT                                      
   \verbatim                                                                                 
    Callback function for responses, VT activation messages ...                                                                                            
      
    VT-Function:                Parameter of       Meaning:
                                ISOVT_MSG_STA_T:      
                                                   
    softkey_activation            wObjectID       key object ID                   
                                  wPara1          parent object ID                
                                  bPara           key number (hard coded)         
                                  lValue          activation code (0, 1, 2, 3(Version 4)) see [1]   
    button_activation             wObjectID       button object ID                
                                  wPara1          parent object ID                
                                  bPara           button key number               
                                  lValue          activation code (0, 1, 2, 3(Version 4)) see [1]
    pointing_event                wPara1          X position in pixel             
                                  wPara2          Y position in pixel             
    VT_select_input_object        wObjectID       input object ID                 
                                  wPara1          Selected/Deselected
                                  wPara2          Bitmask (Version 5 and later)
    VT_esc                        wObjectID       ID of input field where aborted 
                                  iErrorCode      error code see ISO Doku.        
    VT_change_numeric_value       wObjectID       ID of input object              
                                  lValue          new value                       
    VT_change_active_mask         wObjectID       momentan active mask            
                                  iErrorCode      error code see ISO Doku.        
    VT_change_softkey_mask        wObjectID       data or alarm mask object ID    
                                  wPara1          soft key mask object ID         
                                  iErrorCode      error code see ISO Doku         
    VT_change_string_value        wObjectID       ID of String object             
                                  bPara           Length of string                
                                  pabVtData       Pointer to characters 
    ( Version 4 )                 
    VT_onUserLayout_hideShow      wObjectID       Object ID of WM, DM, SKM, KG
                                  wPara2          Hide/show
                                  wPara1          Object ID of second WM, DM, SKM, KG
                                  bPara           Hide/show of second
    get_attribute_value           wObjectID       Object ID
                                  bPara           AID
                                  wPara1          Current value of attribute
                                  iErrorCode      ErrorCode (see F.59)
    ( Version 3 )                 
    preferred_assignment          wObjectID       Auxiliary function object if faulty
                                  iErrorCode      Error code see ISO Doku.
    auxiliary_assign_type_1, .._2 wObjectID       Object ID auxiliary function    
                                  wPara1          Object ID auxiliary input (or input number type 1) 
                                                  0xFFFF for unassign             
                                  wPara2          Type of auxiliary incl. Attribute bits see [2]
                                  bPara           ISO_TRUE: Store as pref. assign, else not (only type 2) 
                                  lValue          Bit 16 - 27: Manufacturer code,         
                                                  Bit  0 - 15 Model Identification code of auxiliary input 
                                                  (only type 2)
                                  pabVtData       only for auxiliary_assign_type_2:
                                                  Pointer to the last/current aux unit ISONAME or 8*0xFF
    auxiliary_input_status_type_2
    aux_input_status_type_1       wObjectID       Object ID Auxiliary function type          
                                  wPara1          Input object ID (type 1 = input number     
                                  lValue          Value 1                                    
                                  wPara2          Value 2                                    
                                  iErrorCode      E_NO_ERR, E_CANMSG_MISSED (Alive of input)
    ( Version 5 )
    auxiliary_capabilities        bPara           Number of auxiliary Units
                                  pabVtData       Pointer to data ( Byte 3 ... )

               [1] Timeout control of softkeys and buttons have to be done of application !
               [2] Attribute bits are given to application as additional information
                   For getting the Auxiliary type application have to mask out it.
   \endverbatim
                                                                                  
   \param[in]     \wpp{pIsoMsgSta, const #ISOVT_MSG_STA_T *}                                                   
                       Pointer on received IS0 messages                                                                                
*/           
static void CbVtMessages( const ISOVT_MSG_STA_T * pIsoMsgSta )
{
   OutputVtMessages(pIsoMsgSta, IsoClientsGetTimeMs());

   switch ( pIsoMsgSta->iVtFunction )
   {
   case button_activation :
   case softkey_activation :
      VTC_handleSoftkeysAndButtons((struct ButtonActivation_S *)(pIsoMsgSta));
      break;
   case VT_change_numeric_value :
      break;
   case VT_change_string_value :
      // Receiving string see Page 3
      //VTC_process_VT_change_string_value(pIsoMsgSta);
      break;
   case auxiliary_assign_type_1 :
       break;
   case auxiliary_assign_type_2 :
#if defined(_LAY6_) && defined(ISO_VTC_GRAPHIC_AUX)
   {  /* Forward assignment message to aux unit */
       iso_s16 s16CfHandle;
       s16CfHandle = IsoVtcGetStatusInfo(pIsoMsgSta->u8Instance, CF_HND); /* or use the stored (s16CfHandleWs) */
       (void)vtcGAux_ForwardAssignment(s16CfHandle, pIsoMsgSta);
   }
#endif // defined(_LAY6_) && defined(ISO_VTC_GRAPHIC_AUX)
       /* Assignment is stored only in case of Byte 10, Bit 7 is zero (use as preferred assignment) */
       if ( pIsoMsgSta->bPara != 0 )
       {
#if(0)
          static iso_s16  iNumberOfFunctions = 0;
          static VT_AUXAPP_T asAuxAss[20];      // AUXINPUTMAX !
          /* Reading the complete actual assignment and storing this in a file or EE */
          IsoAuxAssignmentRead(asAuxAss, &iNumberOfFunctions);
          setAuxAssignment("CF-A-AuxAssignment", asAuxAss, iNumberOfFunctions);
          //IsoAuxWriteAssignToFile(asAuxAss, iNumberOfFunctions);  // Assignment -> File
#else
          if (pIsoMsgSta->wObjectID != NULL_OBJECTID)
          {
             VT_AUXAPP_T auxEntry = { 0 };
             auxEntry.wObjID_Fun = pIsoMsgSta->wObjectID;
             auxEntry.wObjID_Input = pIsoMsgSta->wPara1;
             auxEntry.eAuxType = (VTAUXTYP_e)(pIsoMsgSta->wPara2 & 0x1F);
             auxEntry.wManuCode = (iso_u16)(pIsoMsgSta->lValue >> 16);
             auxEntry.wModelIdentCode = (iso_u16)(pIsoMsgSta->lValue);
             auxEntry.qPrefAssign = pIsoMsgSta->bPara;
             auxEntry.bFuncAttribute = (iso_u8)(pIsoMsgSta->wPara2);
             iso_ByteCpyHuge(auxEntry.baAuxName, &pIsoMsgSta->pabVtData[0], 8);
             updateAuxAssignment("CF-A-AuxAssignment", &auxEntry);
          }
#endif
       }
       break;
   case auxiliary_input_status_type_2 :
	   VTC_handleAux((struct AUX_InputSignalData_T *)(pIsoMsgSta));
       break;
   default:
       break;
   }
}


/* ************************************************************************ */
// Delete stored pool
iso_s16 VTC_PoolDeleteVersion(void)
{  
#if 1 // for V11.xx
      // with V11 - String must be zero terminated (or should be 32 bytes long)
   iso_s16 s16Ret;
   iso_u8 au8VersionString[] = "       "; // We use spaces to delete the last stored pool from flash
   s16Ret = IsoVtcCmd_DeleteVersion(u8_CfVtInstance, au8VersionString);
   return s16Ret;
#else // for V10.xx 
   iso_s16 s16Ret = E_NO_INSTANCE;
   // If called outside of a callback function, we must set the VT client instance before calling any other API function 
   if (IsoWsSetMaskInst(s16_CfHndVtClient) == E_NO_ERR)
   {
      iso_u8 au8VersionString[] = "       "; // We use spaces to delete the last stored pool from flash
      iso_u16 u16WSVersion, u16VTVersion;
      // ISO version string (C-string with termination or 32 bytes; if VT < 5: only 7 Bytes used )
      u16WSVersion = IsoGetVTStatusInfo(WS_VERSION_NR);
      u16VTVersion = IsoGetVTStatusInfo(VT_VERSIONNR);
      if ((u16WSVersion >= VT_V5_SE_UT3) && (u16VTVersion >= VT_V5_SE_UT3))
      {  // 32 byte version label -> use IsoExtendedDeleteVersion()
         #ifdef ISO_VTC_UT3 /* only if compiled for UT3 */
         s16Ret = IsoExtendedDeleteVersion(au8VersionString);
         #endif /* ISO_VTC_UT3 */
      }
      else
      {  // 7 byte version label -> use IsoDeleteVersion()
         s16Ret = IsoDeleteVersion(au8VersionString);
      }
   }

   return s16Ret;
#endif /* 0 */
}

/* ************************************************************************ */
// Reload of objects during "application running"
iso_s16 VTC_PoolReload(void)
{
   iso_s16 iRet = E_NO_ERR;
   /* pointer to the pool data */
   static iso_u8*  pu8PoolData = 0;
   static iso_u32  u32PoolSize = 0UL;
   static iso_u16  u16NumberObjects;

   u32PoolSize = LoadPoolFromFile(POOL_FILENAME, &pu8PoolData);
   u16NumberObjects = IsoGetNumofPoolObjs(pu8PoolData, (iso_s32)u32PoolSize);
   if (IsoVtcPoolReload(u8_CfVtInstance, pu8PoolData, u16NumberObjects))
   {
      //iso_u16 wSKM_Scal = 0u;
      /* Reload ranges */

      /* Manipulating these objects */
      //wSKM_Scal = (iso_u16)IsoVtcPoolReadInfo(u8_CfVtInstance, PoolSoftKeyMaskScalFaktor);

   }
   else
   {
      iRet = E_ERROR_INDI;
   }
   return iRet;
}

/* ************************************************************************ */
// Callback function for setting the preferred assignment
static void CbAuxPrefAssignment(VT_AUXAPP_T asAuxAss[], iso_s16* ps16MaxNumberOfAssigns, ISO_USER_PARAM_T userParam)
{
   iso_s16 s16I;
   iso_s16 s16NumbOfPrefAssigns = 0;
   VT_AUXAPP_T asPrefAss[20];

   /* Reading stored preferred assignment */
   s16NumbOfPrefAssigns = getAuxAssignment("CF-A-AuxAssignment", asPrefAss);

   for (s16I = 0; s16I < s16NumbOfPrefAssigns; s16I++)
   {
      asAuxAss[s16I] = asPrefAss[s16I];
   }
   *ps16MaxNumberOfAssigns = s16NumbOfPrefAssigns;
   (void)userParam;
}


/* ************************************************************************ */
// Multiple VT
iso_s16 VTC_NextVTButtonPressed( )
{
   #define VT_LIST_MAX   5       /* Array size for VT cf handle entries */

   iso_s16 s16NumberOfVTs = 0, s16NumberAct = VT_LIST_MAX, iI = 0;
   iso_s16 s16HndCurrentVT;
   iso_s16 as16HandList[VT_LIST_MAX];

   s16HndCurrentVT = (iso_s16)IsoVtcGetStatusInfo(u8_CfVtInstance, VT_HND);   /* get CF handle of actual VT */
   /* Determine list number of actual VT */
   (void)IsoClientsReadListofExtHandles(virtual_terminal, VT_LIST_MAX, as16HandList, &s16NumberOfVTs);

   for (iI = 0; iI < s16NumberOfVTs; iI++)
   {
      if (s16HndCurrentVT == as16HandList[iI])
      {
         s16NumberAct = iI;
         break;
      }
   }

   /* Select next VT in list */ 
   if ((s16NumberOfVTs > 1) && (s16NumberAct != VT_LIST_MAX))
   {
      ISO_CF_INFO_T sUserVT;
      s16NumberAct++;
      if (s16NumberAct >= s16NumberOfVTs)
      {  /* select first VT in list*/
         s16NumberAct = 0;
      }
      iso_NmGetCfInfo(as16HandList[s16NumberAct], &sUserVT);
      IsoVtcMultipleNextVT(u8_CfVtInstance, CAST_TO_CONST_ISONAME_PTR(&sUserVT.au8Name));
      // NAME could be stored here in EEPROM
      // Application must go into safe state !!!
   }
   return 0;
}

/* ************************************************************************ */
#endif /* _LAY6_ */
/* ************************************************************************ */
