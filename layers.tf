### binaries layer
# data for the binaries_layer layer
data "archive_file" "binaries_layer" {
  type        = "zip"
  source_dir = "${path.module}/layers/binaries/"
  output_path = "${path.module}/binaries.zip"
}

# binaries layer definition
resource "aws_lambda_layer_version" "binaries_layer" {
  filename   = data.archive_file.binaries_layer.output_path
  layer_name = "binaries_layer"
  source_code_hash = filebase64sha256("${data.archive_file.binaries_layer.output_path}")

  compatible_runtimes = ["python3.7", "python3.8", "python3.9"]
}

### python layer 
# contains pynamodb, jsons, jsonschema, smart_open
module "python_libraries_layer" {
  source = "terraform-aws-modules/lambda/aws"

  create_layer = true

  layer_name          = "python_libraries_layer"
  description         = "python libraries"
  compatible_runtimes = ["python3.7", "python3.8", "python3.9"]

  source_path = "${path.module}/layers/python_libraries/"

  store_on_s3 = true
  s3_bucket   = aws_s3_bucket.lambda-layers-bucket.bucket
}
