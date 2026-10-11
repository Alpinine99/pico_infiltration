from django.shortcuts import render
from .models import Reading

def home(request):
    return render(request, "index.html", {"readings": Reading.objects.all()[:20]})

