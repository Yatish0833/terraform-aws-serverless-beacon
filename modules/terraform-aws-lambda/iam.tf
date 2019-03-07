# Create the role.

data "aws_iam_policy_document" "assume_role" {
  statement {
    effect  = "Allow"
    actions = ["sts:AssumeRole"]

    principals {
      type        = "Service"
      identifiers = ["${slice(list("lambda.amazonaws.com", "edgelambda.amazonaws.com"), 0, var.lambda_at_edge ? 2 : 1)}"]
    }
  }
}

resource "aws_iam_role" "lambda" {
  name               = "${var.function_name}"
  assume_role_policy = "${data.aws_iam_policy_document.assume_role.json}"
  tags = "${var.tags}"
}

# Attach a policy for logs.

locals {
  lambda_log_group_arn      = "arn:${data.aws_partition.current.partition}:logs:*:${data.aws_caller_identity.current.account_id}:log-group:/aws/lambda/${var.function_name}"
  lambda_edge_log_group_arn = "arn:${data.aws_partition.current.partition}:logs:*:${data.aws_caller_identity.current.account_id}:log-group:/aws/lambda/us-east-1.${var.function_name}"
  log_group_arns            = ["${slice(list(local.lambda_log_group_arn, local.lambda_edge_log_group_arn), 0, var.lambda_at_edge ? 2 : 1)}"]
}

data "aws_iam_policy_document" "logs" {
  count = "${var.enable_cloudwatch_logs ? 1 : 0}"

  statement {
    effect = "Allow"

    actions = [
      "logs:CreateLogGroup",
    ]

    resources = [
      "*",
    ]
  }

  statement {
    effect = "Allow"

    actions = [
      "logs:CreateLogStream",
      "logs:PutLogEvents",
    ]

    resources = ["${concat(formatlist("%v:*", local.log_group_arns), formatlist("%v:*:*", local.log_group_arns))}"]
  }
}

resource "aws_iam_policy" "logs" {
  count = "${var.enable_cloudwatch_logs ? 1 : 0}"

  name   = "${var.function_name}-logs"
  policy = "${data.aws_iam_policy_document.logs.json}"

}

resource "aws_iam_policy_attachment" "logs" {
  count = "${var.enable_cloudwatch_logs ? 1 : 0}"

  name       = "${var.function_name}-logs"
  roles      = ["${aws_iam_role.lambda.name}"]
  policy_arn = "${aws_iam_policy.logs.arn}"
}

# Attach an additional policy required for the dead letter config.

data "aws_iam_policy_document" "dead_letter" {
  count = "${var.attach_dead_letter_config ? 1 : 0}"

  statement {
    effect = "Allow"

    actions = [
      "sns:Publish",
      "sqs:SendMessage",
    ]

    resources = [
      "${lookup(var.dead_letter_config, "target_arn", "")}",
    ]
  }
}

resource "aws_iam_policy" "dead_letter" {
  count = "${var.attach_dead_letter_config ? 1 : 0}"

  name   = "${var.function_name}-dl"
  policy = "${data.aws_iam_policy_document.dead_letter.json}"
}

resource "aws_iam_policy_attachment" "dead_letter" {
  count = "${var.attach_dead_letter_config ? 1 : 0}"

  name       = "${var.function_name}-dl"
  roles      = ["${aws_iam_role.lambda.name}"]
  policy_arn = "${aws_iam_policy.dead_letter.arn}"
}

# Attach an additional policy required for the VPC config

data "aws_iam_policy_document" "network" {
  statement {
    effect = "Allow"

    actions = [
      "ec2:CreateNetworkInterface",
      "ec2:DescribeNetworkInterfaces",
      "ec2:DeleteNetworkInterface",
    ]

    resources = [
      "*",
    ]
  }
}

resource "aws_iam_policy" "network" {
  count = "${var.attach_vpc_config ? 1 : 0}"

  name   = "${var.function_name}-network"
  policy = "${data.aws_iam_policy_document.network.json}"
}

resource "aws_iam_policy_attachment" "network" {
  count = "${var.attach_vpc_config ? 1 : 0}"

  name       = "${var.function_name}-network"
  roles      = ["${aws_iam_role.lambda.name}"]
  policy_arn = "${aws_iam_policy.network.arn}"
}

# Attach an additional policy if provided.

resource "aws_iam_policy" "additional" {
  count = "${var.attach_policy ? 1 : 0}"

  name   = "${var.function_name}"
  policy = "${var.policy}"
}

resource "aws_iam_policy_attachment" "additional" {
  count = "${var.attach_policy ? 1 : 0}"

  name       = "${var.function_name}"
  roles      = ["${aws_iam_role.lambda.name}"]
  policy_arn = "${aws_iam_policy.additional.arn}"
}
