#pragma once

#include "../../../engine/contracts/field_privacy.h"
#include <msctf.h>

class CMetasequoiaIME;

FanyImeFieldPrivacy::State ResolveInputFieldPrivacy(CMetasequoiaIME *textService, ITfContext *context,
                                                    TfClientId clientId);
