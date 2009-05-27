/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Netscape security libraries.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1994-2000
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Rob Crittenden (rcritten@redhat.com)
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */
#include <stdlib.h>
#include "ckpem.h"
#include "blapi.h"

/*
 * pinstance.c
 *
 * This file implements the NSSCKMDInstance object for the 
 * "PEM objects" cryptoki module.
 */

static PRBool pemInitialized = PR_FALSE;

pemInternalObject **gobj;
int pem_nobjs = 0;
int token_needsLogin[NUM_SLOTS];

PRInt32 size = 0;
PRInt32 count = 0;

#define PEM_ITEM_CHUNK  512

#define PUT_Object(obj,err) \
  { \
    if (count >= size) { \
    gobj = gobj ? \
                nss_ZREALLOCARRAY(gobj, pemInternalObject *, \
                               (size+PEM_ITEM_CHUNK) ) : \
                nss_ZNEWARRAY(NULL, pemInternalObject *, \
                               (size+PEM_ITEM_CHUNK) ) ; \
      if ((pemInternalObject **)NULL == gobj) { \
        err = CKR_HOST_MEMORY; \
        goto loser; \
      } \
      size += PEM_ITEM_CHUNK; \
    } \
    (gobj)[ count ] = (obj); \
    count++; \
  }

/*
 * simple cert decoder to avoid the cost of asn1 engine
 */
static unsigned char *
dataStart(unsigned char *buf, unsigned int length,
          unsigned int *data_length,
          PRBool includeTag, unsigned char *rettag)
{
    unsigned char tag;
    unsigned int used_length = 0;

    tag = buf[used_length++];

    if (rettag) {
        *rettag = tag;
    }

    /* blow out when we come to the end */
    if (tag == 0) {
        return NULL;
    }

    *data_length = buf[used_length++];

    if (*data_length & 0x80) {
        int len_count = *data_length & 0x7f;

        *data_length = 0;

        while (len_count-- > 0) {
            *data_length = (*data_length << 8) | buf[used_length++];
        }
    }

    if (*data_length > (length - used_length)) {
        *data_length = length - used_length;
        return NULL;
    }
    if (includeTag)
        *data_length += used_length;

    return (buf + (includeTag ? 0 : used_length));
}

static int
GetCertFields(unsigned char *cert, int cert_length,
              SECItem * issuer, SECItem * serial, SECItem * derSN,
              SECItem * subject, SECItem * valid, SECItem * subjkey)
{
    unsigned char *buf;
    unsigned int buf_length;
    unsigned char *dummy;
    unsigned int dummylen;

    /* get past the signature wrap */
    buf = dataStart(cert, cert_length, &buf_length, PR_FALSE, NULL);
    if (buf == NULL)
        return SECFailure;
    /* get into the raw cert data */
    buf = dataStart(buf, buf_length, &buf_length, PR_FALSE, NULL);
    if (buf == NULL)
        return SECFailure;
    /* skip past any optional version number */
    if ((buf[0] & 0xa0) == 0xa0) {
        dummy = dataStart(buf, buf_length, &dummylen, PR_FALSE, NULL);
        if (dummy == NULL)
            return SECFailure;
        buf_length -= (dummy - buf) + dummylen;
        buf = dummy + dummylen;
    }
    /* serial number */
    if (derSN) {
        derSN->data =
            dataStart(buf, buf_length, &derSN->len, PR_TRUE, NULL);
    }
    serial->data =
        dataStart(buf, buf_length, &serial->len, PR_FALSE, NULL);
    if (serial->data == NULL)
        return SECFailure;
    buf_length -= (serial->data - buf) + serial->len;
    buf = serial->data + serial->len;
    /* skip the OID */
    dummy = dataStart(buf, buf_length, &dummylen, PR_FALSE, NULL);
    if (dummy == NULL)
        return SECFailure;
    buf_length -= (dummy - buf) + dummylen;
    buf = dummy + dummylen;
    /* issuer */
    issuer->data = dataStart(buf, buf_length, &issuer->len, PR_TRUE, NULL);
    if (issuer->data == NULL)
        return SECFailure;
    buf_length -= (issuer->data - buf) + issuer->len;
    buf = issuer->data + issuer->len;

    /* only wanted issuer/SN */
    if (valid == NULL) {
        return SECSuccess;
    }
    /* validity */
    valid->data = dataStart(buf, buf_length, &valid->len, PR_FALSE, NULL);
    if (valid->data == NULL)
        return SECFailure;
    buf_length -= (valid->data - buf) + valid->len;
    buf = valid->data + valid->len;
    /*subject */
    subject->data =
        dataStart(buf, buf_length, &subject->len, PR_TRUE, NULL);
    if (subject->data == NULL)
        return SECFailure;
    buf_length -= (subject->data - buf) + subject->len;
    buf = subject->data + subject->len;
    /* subject  key info */
    subjkey->data =
        dataStart(buf, buf_length, &subjkey->len, PR_TRUE, NULL);
    if (subjkey->data == NULL)
        return SECFailure;
    buf_length -= (subjkey->data - buf) + subjkey->len;
    buf = subjkey->data + subjkey->len;
    return SECSuccess;
}

pemInternalObject *
CreateObject(CK_OBJECT_CLASS objClass,
             pemObjectType type, SECItem * certDER,
             SECItem * keyDER, char *filename,
             int objid, CK_SLOT_ID slotID)
{
    pemInternalObject *o;
    SECItem subject;
    SECItem issuer;
    SECItem serial;
    SECItem derSN;
    SECItem valid;
    SECItem subjkey;
    char id[16];
    char *nickname;
    int len;

    o = nss_ZNEW(NULL, pemInternalObject);
    if ((pemInternalObject *) NULL == o) {
        return NULL;
    }

    nickname = strrchr(filename, '/');
    if (nickname)
        nickname++;
    else
        nickname = filename;

    switch (objClass) {
    case CKO_CERTIFICATE:
        plog("Creating cert nick %s id %d in slot %ld\n", nickname, objid, slotID);
        memset(&o->u.cert, 0, sizeof(o->u.cert));
        break;
    case CKO_PRIVATE_KEY:
        plog("Creating key id %d in slot %ld\n", objid, slotID);
        memset(&o->u.key, 0, sizeof(o->u.key));
        break;
    case CKO_NETSCAPE_TRUST:
        plog("Creating trust nick %s id %d in slot %ld\n", nickname, objid, slotID);
        memset(&o->u.trust, 0, sizeof(o->u.trust));
        break;
    }
    o->objClass = objClass;
    o->type = type;
    o->slotID = slotID;
    o->derCert = nss_ZNEW(NULL, SECItem);
    o->derCert->data = (void *) nss_ZAlloc(NULL, certDER->len);
    o->derCert->len = certDER->len;
    nsslibc_memcpy(o->derCert->data, certDER->data, certDER->len);

    switch (objClass) {
    case CKO_CERTIFICATE:
    case CKO_NETSCAPE_TRUST:
        GetCertFields(o->derCert->data,
                      o->derCert->len, &issuer, &serial,
                      &derSN, &subject, &valid, &subjkey);

        o->u.cert.subject.data = (void *) nss_ZAlloc(NULL, subject.len);
        o->u.cert.subject.size = subject.len;
        nsslibc_memcpy(o->u.cert.subject.data, subject.data, subject.len);

        o->u.cert.issuer.data = (void *) nss_ZAlloc(NULL, issuer.len);
        o->u.cert.issuer.size = issuer.len;
        nsslibc_memcpy(o->u.cert.issuer.data, issuer.data, issuer.len);

        o->u.cert.serial.data = (void *) nss_ZAlloc(NULL, serial.len);
        o->u.cert.serial.size = serial.len;
        nsslibc_memcpy(o->u.cert.serial.data, serial.data, serial.len);
        break;
    case CKO_PRIVATE_KEY:
        o->u.key.key.privateKey = nss_ZNEW(NULL, SECItem);
        o->u.key.key.privateKey->data =
            (void *) nss_ZAlloc(NULL, keyDER->len);
        o->u.key.key.privateKey->len = keyDER->len;
        nsslibc_memcpy(o->u.key.key.privateKey->data, keyDER->data,
                       keyDER->len);
    }

    o->nickname = (char *) nss_ZAlloc(NULL, strlen(nickname) + 1);
    strcpy(o->nickname, nickname);

    sprintf(id, "%d", objid);

    len = strlen(id) + 1;       /* zero terminate */
    o->id.data = (void *) nss_ZAlloc(NULL, len);
    (void) nsslibc_memcpy(o->id.data, id, len);
    o->id.size = len;

    return o;
}

CK_RV
AddCertificate(char *certfile, char *keyfile, PRBool cacert,
               CK_SLOT_ID slotID)
{
    pemInternalObject *o;
    SECItem certDER;
    CK_RV error = 0;
    int objid, i;
    int nobjs = 0;
    SECItem **objs = NULL;
    char *ivstring = NULL;
    int cipher;

    certDER.data = NULL;
    nobjs = ReadDERFromFile(&objs, certfile, PR_TRUE, &cipher, &ivstring, PR_TRUE /* certs only */);
    if (nobjs <= 0) {
        nss_ZFreeIf(objs);
        return CKR_GENERAL_ERROR;
    }

    /* For now load as many certs as are in the file for CAs only */
    if (cacert) {
        for (i = 0; i < nobjs; i++) {
            char nickname[1024];
            objid = pem_nobjs + 1;

            snprintf(nickname, 1024, "%s - %d", certfile, i);

            o = CreateObject(CKO_CERTIFICATE, pemCert, objs[i], NULL,
                             nickname, 0, slotID);
            if (o == NULL) {
                error = CKR_GENERAL_ERROR;
                goto loser;
            }
            PUT_Object(o, error);
            if (error != CKR_OK)
                goto loser;
            o = NULL;
            pem_nobjs++;

            /* Add the CA trust object */
            o = CreateObject(CKO_NETSCAPE_TRUST, pemTrust, objs[i], NULL,
                             nickname, 0, slotID);
            if (o == NULL) {
                error = CKR_GENERAL_ERROR;
                goto loser;
            }
            PUT_Object(o, error);
            pem_nobjs++;
        }                       /* for */
    } else {
        objid = pem_nobjs + 1;
        o = CreateObject(CKO_CERTIFICATE, pemCert, objs[0], NULL, certfile,
                         objid, slotID);
        if (o == NULL) {
            error = CKR_GENERAL_ERROR;
            goto loser;
        }

        PUT_Object(o, error);

        if (error != CKR_OK)
            goto loser;

        o = NULL;
        pem_nobjs++;

        if (keyfile) {          /* add the private key */
            SECItem **keyobjs = NULL;
            int kobjs = 0;
            kobjs =
                ReadDERFromFile(&keyobjs, keyfile, PR_TRUE, &cipher,
                                &ivstring, PR_FALSE);
            if (kobjs < 1) {
                error = CKR_GENERAL_ERROR;
                goto loser;
            }
            o = CreateObject(CKO_PRIVATE_KEY, pemBareKey, objs[0],
                             keyobjs[0], certfile, objid, slotID);
            if (o == NULL) {
                error = CKR_GENERAL_ERROR;
                goto loser;
            }

            PUT_Object(o, error);
            pem_nobjs++;
        }
    }

    nss_ZFreeIf(objs);
    return CKR_OK;

  loser:
    nss_ZFreeIf(objs);
    nss_ZFreeIf(o);
    return error;
}

CK_RV
pem_Initialize
(
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    NSSUTF8 * configurationData
)
{
    CK_RV rv;
    /* parse the initialization string and initialize CRLInstances */
    char **certstrings = NULL;
    PRInt32 numcerts = 0;
    PRBool status, error = PR_FALSE;
    int i;

    if (pemInitialized) {
        return CKR_OK;
    }
    RNG_RNGInit();

    open_log();

    plog("pem_Initialize\n");

    unsigned char *modparms = NULL;
    if (!fwInstance) {
        return CKR_ARGUMENTS_BAD;
    }

    CK_C_INITIALIZE_ARGS_PTR modArgs =
        NSSCKFWInstance_GetInitArgs(fwInstance);
    if (!modArgs || !modArgs->LibraryParameters) {
        goto done;
    }
    modparms = (unsigned char *) modArgs->LibraryParameters;
    plog("Initialized with %s\n", modparms);

    /*
     * The initialization string format is a space-delimited file of
     * pairs of paths which are delimited by a semi-colon. The first
     * entry of the pair is the path to the certificate file. The
     * second is the path to the key file.
     *
     * CA certificates do not need the semi-colon.
     *
     * Example:
     *  /etc/certs/server.pem;/etc/certs/server.key /etc/certs/ca.pem
     *
     */
    status =
        pem_ParseString((const char *) modparms, ' ', &numcerts,
                        &certstrings);
    if (status == PR_FALSE) {
        return CKR_ARGUMENTS_BAD;
    }

    for (i = 0; i < numcerts && error != PR_TRUE; i++) {
        char *cert = certstrings[i];
        PRInt32 attrcount = 0;
        char **certattrs = NULL;
        status = pem_ParseString(cert, ';', &attrcount, &certattrs);
        if (status == PR_FALSE) {
            error = PR_TRUE;
            break;
        }

        if (error == PR_FALSE) {
            if (attrcount == 1) /* CA certificate */
                rv = AddCertificate(certattrs[0], NULL, PR_TRUE, 0);
            else
                rv = AddCertificate(certattrs[0], certattrs[1], PR_FALSE,
                                    0);

            if (rv != CKR_OK) {
                error = PR_TRUE;
                status = PR_FALSE;
            }
        }
        pem_FreeParsedStrings(attrcount, certattrs);
    }
    pem_FreeParsedStrings(numcerts, certstrings);

    if (status == PR_FALSE) {
        return CKR_ARGUMENTS_BAD;
    }

    for (i = 0; i < NUM_SLOTS; i++)
        token_needsLogin[i] = PR_FALSE;

  done:

    PR_AtomicSet(&pemInitialized, PR_TRUE);

    return CKR_OK;
}

void
pem_Finalize
(
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance
)
{
    plog("pem_Finalize\n");
    if (!pemInitialized)
        return;

    PR_AtomicSet(&pemInitialized, PR_FALSE);

    return;
}

/*
 * NSSCKMDInstance methods
 */

static CK_ULONG
pem_mdInstance_GetNSlots
(
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    CK_RV * pError
)
{
    return (CK_ULONG) NUM_SLOTS;
}

static CK_VERSION
pem_mdInstance_GetCryptokiVersion
(
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance
)
{
    return pem_CryptokiVersion;
}

static NSSUTF8 *
pem_mdInstance_GetManufacturerID
(
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    CK_RV * pError
)
{
    return (NSSUTF8 *) pem_ManufacturerID;
}

static NSSUTF8 *
pem_mdInstance_GetLibraryDescription
(
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    CK_RV * pError
)
{
    return (NSSUTF8 *) pem_LibraryDescription;
}

static CK_VERSION
pem_mdInstance_GetLibraryVersion
(
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance
)
{
    return pem_LibraryVersion;
}

static CK_RV
pem_mdInstance_GetSlots
(
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    NSSCKMDSlot * slots[]
)
{
    int i;
    CK_RV pError;

    for (i = 0; i < NUM_SLOTS; i++) {
        slots[i] = (NSSCKMDSlot *) pem_NewSlot(fwInstance, &pError);
        if (pError != CKR_OK)
            return pError;
    }
    return CKR_OK;
}

CK_BBOOL
pem_mdInstance_ModuleHandlesSessionObjects
(
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance
)
{
    return CK_TRUE;
}

NSS_IMPLEMENT_DATA const NSSCKMDInstance
pem_mdInstance = {
    (void *) NULL, /* etc */
    pem_Initialize, /* Initialize */
    pem_Finalize, /* Finalize */
    pem_mdInstance_GetNSlots,
    pem_mdInstance_GetCryptokiVersion,
    pem_mdInstance_GetManufacturerID,
    pem_mdInstance_GetLibraryDescription,
    pem_mdInstance_GetLibraryVersion,
    pem_mdInstance_ModuleHandlesSessionObjects,
    pem_mdInstance_GetSlots,
    NULL, /* WaitForSlotEvent */
    (void *) NULL /* null terminator */
};
