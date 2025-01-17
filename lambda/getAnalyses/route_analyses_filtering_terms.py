import json
import os
import csv

from smart_open import open as sopen

from apiutils.api_response import bundle_response
from athena.common import run_custom_query


BEACON_API_VERSION = os.environ['BEACON_API_VERSION']
BEACON_ID = os.environ['BEACON_ID']
TERMS_TABLE = os.environ['TERMS_TABLE']
METADATA_BUCKET = os.environ['METADATA_BUCKET']


def get_terms(terms, skip, limit):
    response =     {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "info": {
            "message": "Endpoint is not defined in schema!"
        },
        "meta": {
            "apiVersion": BEACON_API_VERSION,
            "beaconId": BEACON_ID,
            "returnedSchemas": [],
            "receivedRequestSummary": {
                "apiVersion": "",  # TODO
                "requestedSchemas": [],  # TODO
                "pagination": {
                    "skip": skip,
                    "limit": limit,
                },
                "requestedGranularity": "record"  # TODO
            }
        },
        "response": {
            "filteringTerms": terms,
            # "resources": [
            #     {
            #         "id": "NA",
            #         "iriPrefix": "NA",
            #         "name": "NA",
            #         "namespacePrefix": "NA",
            #         "url": "NA",
            #         "version": "TBD"
            #     }
            # ]
        }
    }

    return bundle_response(200, response)


def route(event):
    print('Event received', event)
    if event['httpMethod'] == 'GET':
        params = event.get('queryStringParameters', None) or dict()
        print(f"Query params {params}")
        apiVersion = params.get("apiVersion", BEACON_API_VERSION)
        requestedSchemas = params.get("requestedSchemas", [])
        skip = params.get("skip", 0)
        limit = params.get("limit", 100)
    if event['httpMethod'] == 'POST':
        params = json.loads(event.get('body') or "{}")
        print(f"POST params {params}")
        meta = params.get("meta", dict())
        query = params.get("query", dict())
        # meta data
        apiVersion = meta.get("apiVersion", BEACON_API_VERSION)
        requestedSchemas = meta.get("requestedSchemas", [])
        # pagination
        pagination = query.get("pagination", dict())
        skip = pagination.get("skip", 0)
        limit = pagination.get("limit", 100)
        # validate query request
        # validator = jsonschema.Draft202012Validator(requestSchemaJSON['g_variant'])
        # print(validator.schema)
        # if errors := sorted(validator.iter_errors(requestParameters), key=lambda e: e.path):
        #     return bad_request(errorMessage= "\n".join([error.message for error in errors]))
            # raise error

    query = f'''
    SELECT DISTINCT term, label, type 
    FROM "{TERMS_TABLE}"
    WHERE "kind"='analyses'
    ORDER BY term
    OFFSET {skip}
    LIMIT {limit};
    '''

    print('Performing query \n', query)
        
    exec_id = run_custom_query(query, return_id=True)
    filteringTerms = []
    
    with sopen(f's3://{METADATA_BUCKET}/query-results/{exec_id}.csv') as s3f:
        reader = csv.reader(s3f)

        for n, row in enumerate(reader):
            if n==0:
                continue
            term, label, typ = row
            filteringTerms.append({
                "id": term,
                "label": label,
                "type": typ
            })

    response = get_terms(
        filteringTerms,
        skip=skip,
        limit=limit
    )

    print('Returning Response: {}'.format(json.dumps(response)))
    return response
