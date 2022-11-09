from collections import defaultdict
import json
import os
import base64
import csv
import sys

from smart_open import open as sopen

from variantutils.search_variants import perform_variant_search
from apiutils.api_response import bundle_response, fetch_from_cache
import apiutils.responses as responses
import apiutils.entries as entries
from dynamodb.variant_queries import get_job_status, JobStatus, VariantQuery, get_current_time_utc
from athena.common import entity_search_conditions, run_custom_query
from athena.dataset import Dataset


BEACON_API_VERSION = os.environ['BEACON_API_VERSION']
DATASETS_TABLE = os.environ['DATASETS_TABLE']
ANALYSES_TABLE = os.environ['ANALYSES_TABLE']
METADATA_DATABASE = os.environ['METADATA_DATABASE']
METADATA_BUCKET = os.environ['METADATA_BUCKET']
csv.field_size_limit(sys.maxsize)


def datasets_query(conditions, assembly_id):
    query = f'''
    SELECT D.id, D._vcflocations, D._vcfchromosomemap, array_agg(A._vcfsampleid) as samples
    FROM "{METADATA_DATABASE}"."{ANALYSES_TABLE}" A
    JOIN "{METADATA_DATABASE}"."{DATASETS_TABLE}" D
    ON A._datasetid = D.id
    {conditions} 
    AND D._assemblyid='{assembly_id}' 
    GROUP BY D.id, D._vcflocations, D._vcfchromosomemap 
    '''
    return query


def datasets_query_fast(assembly_id):
    query = f'''
    SELECT id, _vcflocations, _vcfchromosomemap
    FROM "{METADATA_DATABASE}"."{DATASETS_TABLE}"
    WHERE _assemblyid='{assembly_id}' 
    '''
    return query


def parse_array(exec_id):
        datasets = []
        samples = []

        var_list = list()
        case_map = { k.lower(): k for k in Dataset().__dict__.keys() }

        with sopen(f's3://{METADATA_BUCKET}/query-results/{exec_id}.csv') as s3f:
            reader = csv.reader(s3f)

            for n, row in enumerate(reader):
                if n==0:
                    var_list = row
                else:
                    instance = Dataset()
                    for attr, val in zip(var_list, row):
                        if attr == 'samples':
                            samples.append(val.replace('[', '').replace(']', '').split(', '))
                        elif attr not in case_map:
                            continue
                        else:
                            try:
                                val = json.loads(val)
                            except:
                                val = val
                            instance.__dict__[case_map[attr]] = val
                    datasets.append(instance)

        return datasets, samples


def route(event, query_id):
    if event['httpMethod'] == 'GET':
        params = event['queryStringParameters'] or dict()
        print(f"Query params {params}")
        apiVersion = params.get("apiVersion", BEACON_API_VERSION)
        requestedSchemas = params.get("requestedSchemas", [])
        skip = params.get("skip", 0)
        limit = params.get("limit", 100)
        includeResultsetResponses = params.get("includeResultsetResponses", 'NONE')
        start = params.get("start", [])
        end = params.get("end", [])
        assemblyId = params.get("assemblyId", None)
        referenceName = params.get("referenceName", None)
        referenceBases = params.get("referenceBases", None)
        alternateBases = params.get("alternateBases", None)
        variantMinLength = params.get("variantMinLength", 0)
        variantMaxLength = params.get("variantMaxLength", -1)
        variantType = params.get("variantType", None)
        allele = params.get("allele", None)
        geneid = params.get("geneid", None)
        aminoacidchange = params.get("aminoacidchange", None)
        filters = params.get("filters", [])
        requestedGranularity = params.get("requestedGranularity", "boolean")

    if event['httpMethod'] == 'POST':
        params = json.loads(event['body']) or dict()
        print(f"POST params {params}")
        meta = params.get("meta", dict())
        query = params.get("query", dict()) or dict()
        # meta data
        apiVersion = meta.get("apiVersion", BEACON_API_VERSION)
        requestedSchemas = meta.get("requestedSchemas", [])
        # query data
        requestParameters = query.get("requestParameters", None)
        requestedGranularity = query.get("requestedGranularity", "boolean")
        # pagination
        pagination = query.get("pagination", dict())
        skip = pagination.get("skip", 0)
        limit = pagination.get("limit", 100)
        currentPage = pagination.get("currentPage", None)
        previousPage = pagination.get("previousPage", None)
        nextPage = pagination.get("nextPage", None)
        # query request params
        requestParameters = query.get("requestParameters", dict())
        start = requestParameters.get("start", None)
        end = requestParameters.get("end", None)
        assemblyId = requestParameters.get("assemblyId", None)
        referenceName = requestParameters.get("referenceName", None)
        referenceBases = requestParameters.get("referenceBases", None)
        alternateBases = requestParameters.get("alternateBases", None)
        variantMinLength = requestParameters.get("variantMinLength", 0)
        variantMaxLength = requestParameters.get("variantMaxLength", -1)
        allele = requestParameters.get("allele", None)
        geneId = requestParameters.get("geneId", None)
        aminoacidChange = requestParameters.get("aminoacidChange", None)
        filters = query.get("filters", [])
        variantType = requestParameters.get("variantType", None)
        includeResultsetResponses = query.get("includeResultsetResponses", 'NONE')

        
    check_all = includeResultsetResponses in ('HIT', 'ALL')
    status = get_job_status(query_id)

    if status == JobStatus.NEW:
        conditions = entity_search_conditions(filters, 'analyses', id_modifier='A.id')
        
        if conditions:
            query = datasets_query(conditions, assemblyId)
            exec_id = run_custom_query(query, return_id=True)
            print('execution id ', exec_id)
            datasets, samples = parse_array(exec_id)
        else:
            query = datasets_query_fast(assemblyId)
            datasets = Dataset.get_by_query(query)
            samples = []

        query_responses = perform_variant_search(
            datasets=datasets,
            referenceName=referenceName,
            referenceBases=referenceBases,
            alternateBases=alternateBases,
            start=start,
            end=end,
            variantType=variantType,
            variantMinLength=variantMinLength,
            variantMaxLength=variantMaxLength,
            requestedGranularity=requestedGranularity,
            includeResultsetResponses=includeResultsetResponses,
            query_id=query_id,
            dataset_samples=samples
        )
    
        variants = set()
        results = list()
        found = set()
        # key=pos-ref-alt
        # val=counts
        variant_call_counts = defaultdict(int)
        variant_allele_counts = defaultdict(int)
        exists = False

        for query_response in query_responses:
            exists = exists or query_response.exists

            if exists:
                if requestedGranularity == 'boolean':
                    break
                if check_all:
                    variants.update(query_response.variants)

                    for variant in query_response.variants:
                        chrom, pos, ref, alt, typ = variant.split('\t')
                        idx = f'{pos}_{ref}_{alt}'
                        variant_call_counts[idx] += query_response.call_count
                        variant_allele_counts[idx] += query_response.all_alleles_count
                        internal_id = f'{assemblyId}\t{chrom}\t{pos}\t{ref}\t{alt}'

                        if internal_id not in found:
                            results.append(entries.get_variant_entry(base64.b64encode(f'{internal_id}'.encode()).decode(), assemblyId, ref, alt, int(pos), int(pos) + len(alt), typ))
                            found.add(internal_id)

        query = VariantQuery.get(query_id)
        query.update(actions=[
            VariantQuery.complete.set(True), 
            VariantQuery.elapsedTime.set((get_current_time_utc() - query.startTime).total_seconds())
        ])

        if requestedGranularity == 'boolean':
            response = responses.get_boolean_response(exists=exists)
            print('Returning Response: {}'.format(json.dumps(response)))
            return bundle_response(200, response, query_id)

        if requestedGranularity == 'count':
            response = responses.get_counts_response(exists=exists, count=len(variants))
            print('Returning Response: {}'.format(json.dumps(response)))
            return bundle_response(200, response, query_id)

        if requestedGranularity in ('record', 'aggregated'):
            response = responses.get_result_sets_response(
                setType='genomicVariant', 
                reqPagination=responses.get_pagination_object(skip, limit),
                exists=exists,
                total=len(variants),
                results=results
            )
            print('Returning Response: {}'.format(json.dumps(response)))
            return bundle_response(200, response, query_id)
    
    elif status == JobStatus.RUNNING:
        response = responses.get_boolean_response(exists=False, info={'message': 'Query still running.'})
        print('Returning Response: {}'.format(json.dumps(response)))
        return bundle_response(200, response)
    
    else:
        response = fetch_from_cache(query_id)
        print('Returning Response: {}'.format(json.dumps(response)))
        return bundle_response(200, response)


if __name__ == '__main__':

    from utils.chrom_matching import get_matching_chromosome

    datasets, samples = parse_array('db94bb9a-fb07-488a-a36c-c0ca899274fc')

    print(len(datasets), len(samples))

    for x, y in zip(datasets, samples):
        vcf_chromosomes = {vcfm['vcf']: get_matching_chromosome(
            vcfm['chromosomes'], '5') for dataset in datasets for vcfm in dataset._vcfChromosomeMap}

        vcf_locations = {
            vcf: vcf_chromosomes[vcf]
            for vcf in x._vcfLocations
            if vcf_chromosomes[vcf]
        }
        print(vcf_locations, y[0])