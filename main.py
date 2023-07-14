"""
Modified from
https://github.com/facebookresearch/LeViT/blob/main/main.py
"""

import argparse
import numpy as np
import os
import time
import datetime
from collections import defaultdict, deque
from PIL import Image

import torch
import torch.distributed as dist
import torch.backends.cudnn as cudnn
from torchvision import datasets, transforms

from timm.models import create_model
from timm.utils import accuracy
from timm.data.constants import IMAGENET_DEFAULT_MEAN, IMAGENET_DEFAULT_STD
from speed_test import replace_batchnorm

import levit
import levit_c
import efficientformer_v2
import swiftformer
import edgenext
import edgenext_bn_hs
import emo
import mobilevit
import mobilevit_v2

class SmoothedValue(object):
    """Track a series of values and provide access to smoothed values over a
    window or the global series average.
    """

    def __init__(self, window_size=50, fmt=None):
        if fmt is None:
            fmt = "{median:.4f} ({global_avg:.4f})"
        self.deque = deque(maxlen=window_size)
        self.total = 0.0
        self.count = 0
        self.fmt = fmt

    def update(self, value, n=1):
        self.deque.append(value)
        self.count += n
        self.total += value * n

    def synchronize_between_processes(self):
        """
        Warning: does not synchronize the deque!
        """
        if not dist.is_available() or not dist.is_initialized():
            return
        t = torch.tensor([self.count, self.total],
                         dtype=torch.float64, device='cuda')
        dist.barrier()
        dist.all_reduce(t)
        t = t.tolist()
        self.count = int(t[0])
        self.total = t[1]

    @property
    def median(self):
        d = torch.tensor(list(self.deque))
        return d.median().item()

    @property
    def avg(self):
        d = torch.tensor(list(self.deque), dtype=torch.float32)
        return d.mean().item()

    @property
    def global_avg(self):
        return self.total / self.count

    @property
    def max(self):
        return max(self.deque)

    @property
    def value(self):
        return self.deque[-1]

    def __str__(self):
        return self.fmt.format(
            median=self.median,
            avg=self.avg,
            global_avg=self.global_avg,
            max=self.max,
            value=self.value)


class MetricLogger(object):
    def __init__(self, delimiter="\t"):
        self.meters = defaultdict(SmoothedValue)
        self.delimiter = delimiter

    def update(self, **kwargs):
        for k, v in kwargs.items():
            if isinstance(v, torch.Tensor):
                v = v.item()
            assert isinstance(v, (float, int))
            self.meters[k].update(v)

    def __getattr__(self, attr):
        if attr in self.meters:
            return self.meters[attr]
        if attr in self.__dict__:
            return self.__dict__[attr]
        raise AttributeError("'{}' object has no attribute '{}'".format(
            type(self).__name__, attr))

    def __str__(self):
        loss_str = []
        for name, meter in self.meters.items():
            loss_str.append(
                "{}: {}".format(name, str(meter))
            )
        return self.delimiter.join(loss_str)

    def synchronize_between_processes(self):
        for meter in self.meters.values():
            meter.synchronize_between_processes()

    def add_meter(self, name, meter):
        self.meters[name] = meter

    def log_every(self, iterable, print_freq, header=None):
        i = 0
        if not header:
            header = ''
        start_time = time.time()
        end = time.time()
        iter_time = SmoothedValue(fmt='{avg:.4f}')
        data_time = SmoothedValue(fmt='{avg:.4f}')
        space_fmt = ':' + str(len(str(len(iterable)))) + 'd'
        log_msg = [
            header,
            '[{0' + space_fmt + '}/{1}]',
            'eta: {eta}',
            '{meters}',
            'time: {time}',
            'data: {data}'
        ]
        if torch.cuda.is_available():
            log_msg.append('max mem: {memory:.0f}')
        log_msg = self.delimiter.join(log_msg)
        MB = 1024.0 * 1024.0
        for obj in iterable:
            data_time.update(time.time() - end)
            yield obj
            iter_time.update(time.time() - end)
            if i % print_freq == 0 or i == len(iterable) - 1:
                eta_seconds = iter_time.global_avg * (len(iterable) - i)
                eta_string = str(datetime.timedelta(seconds=int(eta_seconds)))
                if torch.cuda.is_available():
                    print(log_msg.format(
                        i, len(iterable), eta=eta_string,
                        meters=str(self),
                        time=str(iter_time), data=str(data_time),
                        memory=torch.cuda.max_memory_allocated() / MB))
                else:
                    print(log_msg.format(
                        i, len(iterable), eta=eta_string,
                        meters=str(self),
                        time=str(iter_time), data=str(data_time)))
            i += 1
            end = time.time()
        total_time = time.time() - start_time
        total_time_str = str(datetime.timedelta(seconds=int(total_time)))
        print('{} Total time: {} ({:.4f} s / it)'.format(
            header, total_time_str, total_time / len(iterable)))

def get_args_parser():
    parser = argparse.ArgumentParser(
        'LeViT training and evaluation script', add_help=False)
    parser.add_argument('--batch-size', default=64, type=int)

    # Model parameters
    parser.add_argument('--model', default='LeViT_256', type=str, metavar='MODEL',
                        help='Name of model to train')

    parser.add_argument('--weights', default='', type=str, help='weights path')
    parser.add_argument('--fuse', action='store_true', default=False)
    parser.add_argument('--model-ema', action='store_true', default=False)

    parser.add_argument('--usi_eval', action='store_true', default=False,
                        help="Enable it when testing USI model.")
    parser.add_argument('--use_amp', action='store_true', default=False,
                        help="Use PyTorch's AMP (Automatic Mixed Precision) or not")
    parser.add_argument('--num_workers', default=10, type=int)

    # Dataset parameters
    parser.add_argument('--data-path', default='../imagenet', type=str, help='dataset path')
    parser.add_argument('--device', default='cuda',
                        help='device to use for training / testing')
    parser.add_argument('--pin-mem', action='store_true',
                        help='Pin CPU memory in DataLoader for more efficient (sometimes) transfer to GPU.')
    parser.add_argument('--no-pin-mem', action='store_false', dest='pin_mem',
                        help='')
    parser.set_defaults(pin_mem=True)
    return parser

def pil_loader_RGB(path: str) -> Image.Image:
    with open(path, "rb") as f:
        img = Image.open(f)
        return img.convert("RGB")

# For mobilevit, images are expected to be in BGR pixel order, not RGB.
# https://www.adamsmith.haus/python/answers/how-to-rotate-image-colors-from-rgb-to-bgr-in-python
def pil_loader_BGR(path: str) -> Image.Image:
    with open(path, "rb") as f:
        img = Image.open(f)
        R, G, B = img.convert("RGB").split()
        return Image.merge("RGB", (B, G, R))

def build_dataset(args):
    t = []
    is_mobilevit = "mobilevit" in args.model
    is_edgenext  = "edgenext" in args.model

    size = 256
    if args.usi_eval: # for EdgeNeXt
        size = int(256 / 0.95)
    elif is_mobilevit:
        # https://huggingface.co/apple/mobilevit-small
        size = 288
    elif is_edgenext:
        size = int(256 * 256 / 224)
    t.append(
        # to maintain same ratio w.r.t. 224 images
        transforms.Resize(size, interpolation=transforms.InterpolationMode.BICUBIC),
    )

    input_size = 224
    if is_mobilevit or is_edgenext:
        input_size = 256

    t.append(transforms.CenterCrop(input_size))

    t.append(transforms.ToTensor())
    if not is_mobilevit:
        t.append(transforms.Normalize(IMAGENET_DEFAULT_MEAN, IMAGENET_DEFAULT_STD))

    # args.data_set == 'IMNET':
    root = os.path.join(args.data_path)
    is_mobilevitv1 = "mobilevit_" in args.model
    loader = pil_loader_BGR if is_mobilevitv1 else pil_loader_RGB
    dataset = datasets.ImageFolder(root, transform=transforms.Compose(t), loader=loader)
    nb_classes = 1000

    return dataset, nb_classes

@torch.no_grad()
def evaluate(data_loader, model, device, use_amp=False):
    criterion = torch.nn.CrossEntropyLoss()

    metric_logger = MetricLogger(delimiter="  ")
    header = 'Test:'

    # switch to evaluation mode
    model.eval()

    for images, target in metric_logger.log_every(data_loader, 50, header):
        batch_size = images.shape[0]
        if batch_size == 1: target = target * 20
        images = images.to(device, non_blocking=True)
        target = target.to(device, non_blocking=True)

        # compute output
        if use_amp: # will acc val speed
            with torch.cuda.amp.autocast():
                output = model(images)
                loss = criterion(output, target)
        else:
            output = model(images)
            loss = criterion(output, target)

        acc1, acc5 = accuracy(output, target, topk=(1, 5))
        metric_logger.update(loss=loss.item())
        metric_logger.meters['acc1'].update(acc1.item(), n=batch_size)
        metric_logger.meters['acc5'].update(acc5.item(), n=batch_size)

    # gather the stats from all processes
    metric_logger.synchronize_between_processes()
    print('* Acc@1 {top1.global_avg:.3f} Acc@5 {top5.global_avg:.3f} loss {losses.global_avg:.3f}'
          .format(top1=metric_logger.acc1, top5=metric_logger.acc5, losses=metric_logger.loss))
    print(output.mean().item(), output.std().item())

    return {k: meter.global_avg for k, meter in metric_logger.meters.items()}

def main(args):
    device = torch.device(args.device)

    print(f"Creating model: {args.model}")
    model = create_model(args.model)

    # load model weights
    weights_dict = torch.load(args.weights, map_location="cpu")
    # print(weights_dict.keys())

    if "state_dict" in weights_dict:
        args.usi_eval = True
        weights_dict = weights_dict["state_dict"]
    elif args.model_ema: # for EdgeNeXt
        weights_dict = weights_dict["model_ema"]
    elif "model" in weights_dict:
        weights_dict = weights_dict["model"]

    model.load_state_dict(weights_dict)

    if args.fuse:
        replace_batchnorm(model)  # TODO: acc val speed & acc

    model.to(device)

    if args.batch_size == 1: args.num_workers = 1
    print(args)

    n_parameters = sum(p.numel() for p in model.parameters() if p.requires_grad)
    print('number of params:', n_parameters)

    # fix the seed for reproducibility
    torch.manual_seed(0)
    np.random.seed(0)

    cudnn.benchmark = True
    dataset_val, args.nb_classes = build_dataset(args=args)
    sampler_val = torch.utils.data.SequentialSampler(dataset_val)
    data_loader_val = torch.utils.data.DataLoader(
        dataset_val,
        sampler=sampler_val,
        batch_size=int(1.5 * args.batch_size),
        num_workers=args.num_workers, # acc val speed
        pin_memory=args.pin_mem,
        drop_last=False
    )

    test_stats = evaluate(data_loader_val, model, device, use_amp=args.use_amp)
    print(f"Accuracy on {len(dataset_val)} test images: {test_stats['acc1']:.1f}%")

if __name__ == '__main__':
    parser = argparse.ArgumentParser('EdgeTransformerPerf evaluation script', parents=[get_args_parser()])
    args = parser.parse_args()
    main(args)
