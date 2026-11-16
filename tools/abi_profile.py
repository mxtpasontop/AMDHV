"""Validate the manually reviewed ABI policy; never infer or approve evidence."""
import json
from pathlib import Path
import re
import uuid


def validate(profile):
    if not isinstance(profile, dict) or profile.get('schema') != 1:
        raise ValueError('unsupported ABI profile schema')
    for kind in ('kernel', 'applehv'):
        image = profile.get(kind)
        if not isinstance(image, dict):
            raise ValueError('missing ABI image: '+kind)
        identity = image.get('uuid')
        if not isinstance(identity, str) or str(uuid.UUID(identity)).upper() != identity:
            raise ValueError('invalid ABI UUID: '+kind)
        if not isinstance(image.get('sha256'), str) or not re.fullmatch('[0-9a-f]{64}', image['sha256']):
            raise ValueError('invalid ABI image hash: '+kind)
        evidence = image.get('evidence')
        if not isinstance(evidence, list) or not evidence or len(evidence) > 256:
            raise ValueError('missing/excessive ABI evidence: '+kind)
        seen = set()
        for item in evidence:
            if not isinstance(item, dict) or not isinstance(item.get('symbol'), str) or not re.fullmatch('_[A-Za-z0-9_$]+', item['symbol']):
                raise ValueError('invalid ABI symbol: '+kind)
            offset = item.get('offset')
            if type(offset) is not int or not 0 <= offset < (1 << 24):
                raise ValueError('invalid ABI evidence offset')
            data = item.get('bytes')
            if not isinstance(data, str) or not re.fullmatch('(?:[0-9a-f]{2}){1,64}', data):
                raise ValueError('invalid ABI instruction span')
            if not isinstance(item.get('contract'), str) or not item['contract'].strip():
                raise ValueError('missing ABI contract explanation')
            key = (item['symbol'], offset)
            if key in seen:
                raise ValueError('duplicate ABI evidence')
            seen.add(key)
    return profile


def load(path):
    return validate(json.loads(Path(path).read_text()))
